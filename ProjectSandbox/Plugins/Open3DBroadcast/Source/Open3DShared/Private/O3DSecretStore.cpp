// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSecretStore.h"

#include "CoreGlobals.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeLock.h"

#if WITH_EDITOR
#include "Misc/Base64.h"
#include "Misc/ConfigCacheIni.h"
#endif

namespace
{
	/** Keeps a composite key usable as an ini key: [A-Za-z0-9._-] and '|' as the separator. */
	FString SanitizeKeyPart(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len());
		for (const TCHAR C : In)
		{
			const bool bAllowed = (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9')
				|| C == '.' || C == '_' || C == '-';
			Out.AppendChar(bAllowed ? C : TCHAR('_'));
		}
		return Out;
	}

#if WITH_EDITOR
	/**
	 * R3: remembered secrets in the per-user EditorPerProjectUserSettings ini under Saved/Config
	 * (GEditorPerProjectIni), never a Default*.ini. Values are Base64-encoded so arbitrary
	 * characters survive the ini format; this is an encoding, not protection.
	 *
	 * ADR 0004 describes R3 as a UCLASS(Config = EditorPerProjectUserSettings). This uses GConfig on
	 * the same file instead, so packaged builds carry no config class for an editor-only file.
	 * The D9 editor module is the long-term home.
	 */
	class FO3DEditorSecretUserStore final : public IO3DSecretUserStore
	{
	public:
		virtual bool Load(const FString& CompositeKey, FString& OutValue) const override
		{
			if (!GConfig || GEditorPerProjectIni.IsEmpty())
			{
				return false;
			}

			FString Encoded;
			if (!GConfig->GetString(Section, *CompositeKey, Encoded, GEditorPerProjectIni) || Encoded.IsEmpty())
			{
				return false;
			}

			return FBase64::Decode(Encoded, OutValue) && !OutValue.IsEmpty();
		}

		virtual void Save(const FString& CompositeKey, const FString& Value) override
		{
			if (!GConfig || GEditorPerProjectIni.IsEmpty())
			{
				return;
			}

			GConfig->SetString(Section, *CompositeKey, *FBase64::Encode(Value), GEditorPerProjectIni);
			GConfig->Flush(false, GEditorPerProjectIni);
		}

		virtual void Remove(const FString& CompositeKey) override
		{
			if (!GConfig || GEditorPerProjectIni.IsEmpty())
			{
				return;
			}

			if (GConfig->RemoveKey(Section, *CompositeKey, GEditorPerProjectIni))
			{
				GConfig->Flush(false, GEditorPerProjectIni);
			}
		}

	private:
		static constexpr const TCHAR* Section = TEXT("Open3DBroadcast.RememberedSecrets");
	};
#endif // WITH_EDITOR
}

FO3DSecretStore::FO3DSecretStore(FEnvironmentReader InEnvironmentReader, TSharedPtr<IO3DSecretUserStore> InUserStore)
	: EnvironmentReader(MoveTemp(InEnvironmentReader))
	, UserStore(MoveTemp(InUserStore))
{
	if (!EnvironmentReader)
	{
		EnvironmentReader = [](const FString& Name) -> FString
		{
			return FPlatformMisc::GetEnvironmentVariable(*Name);
		};
	}
}

FO3DSecretStore::~FO3DSecretStore() = default;

FO3DSecretStore& FO3DSecretStore::Get()
{
#if WITH_EDITOR
	static FO3DSecretStore Instance(nullptr, MakeShared<FO3DEditorSecretUserStore>());
#else
	// Packaged games never read or write R3 (ADR 0004 item 8).
	static FO3DSecretStore Instance(nullptr, nullptr);
#endif
	return Instance;
}

FString FO3DSecretStore::NormalizeProfile(const FString& Profile)
{
	const FString Trimmed = Profile.TrimStartAndEnd();
	return Trimmed.IsEmpty() ? FString(DefaultProfile()) : Trimmed;
}

FString FO3DSecretStore::MakeCredentialProfileOptionKey(const FString& Transport)
{
	return Transport.TrimStartAndEnd().ToLower() + TEXT(".credentialProfile");
}

TArray<FString> FO3DSecretStore::GetEnvVarCandidates(const FString& EnvVarBaseName, const FString& Profile)
{
	TArray<FString> Candidates;
	const FString Base = EnvVarBaseName.TrimStartAndEnd();
	if (Base.IsEmpty())
	{
		return Candidates;
	}

	const FString NormalizedProfile = NormalizeProfile(Profile);
	if (!NormalizedProfile.Equals(DefaultProfile(), ESearchCase::IgnoreCase))
	{
		FString Suffix;
		Suffix.Reserve(NormalizedProfile.Len());
		for (const TCHAR C : NormalizedProfile.ToUpper())
		{
			const bool bAllowed = (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
			Suffix.AppendChar(bAllowed ? C : TCHAR('_'));
		}
		Candidates.Add(Base + TEXT("__") + Suffix);
	}

	Candidates.Add(Base);
	return Candidates;
}

FString FO3DSecretStore::MakeCompositeKey(const FString& Transport, const FString& Profile, const FString& Key)
{
	return SanitizeKeyPart(Transport.TrimStartAndEnd().ToLower())
		+ TEXT("|") + SanitizeKeyPart(NormalizeProfile(Profile).ToLower())
		+ TEXT("|") + SanitizeKeyPart(Key);
}

bool FO3DSecretStore::CanUseUserStore() const
{
	// GConfig-backed R3 is touched from the game thread only.
	return UserStore.IsValid() && IsInGameThread();
}

bool FO3DSecretStore::LoadRemembered(const FString& CompositeKey, FString& OutValue) const
{
	return CanUseUserStore() && UserStore->Load(CompositeKey, OutValue);
}

bool FO3DSecretStore::ResolveEnvironment(const FString& EnvVarBaseName, const FString& Profile, FString& OutValue, FString& OutName) const
{
	if (!EnvironmentReader)
	{
		return false;
	}

	for (const FString& Candidate : GetEnvVarCandidates(EnvVarBaseName, Profile))
	{
		FString Value = EnvironmentReader(Candidate);
		if (!Value.IsEmpty())
		{
			OutValue = MoveTemp(Value);
			OutName = Candidate;
			return true;
		}
	}
	return false;
}

void FO3DSecretStore::Set(const FString& Transport, const FString& Profile, const FString& Key, const FString& Value,
	EO3DSecretPersistence Persistence)
{
	if (Key.IsEmpty())
	{
		return;
	}

	if (Value.IsEmpty())
	{
		Clear(Transport, Profile, Key);
		return;
	}

	const FString CompositeKey = MakeCompositeKey(Transport, Profile, Key);
	{
		FScopeLock Lock(&Mutex);
		SessionValues.Add(CompositeKey, Value);
	}

	if (Persistence == EO3DSecretPersistence::RememberOnThisMachine && CanUseUserStore())
	{
		UserStore->Save(CompositeKey, Value);
	}
}

bool FO3DSecretStore::SetPersistence(const FString& Transport, const FString& Profile, const FString& Key, EO3DSecretPersistence Persistence)
{
	const FString CompositeKey = MakeCompositeKey(Transport, Profile, Key);

	FString SessionValue;
	{
		FScopeLock Lock(&Mutex);
		if (const FString* Existing = SessionValues.Find(CompositeKey))
		{
			SessionValue = *Existing;
		}
	}

	if (!CanUseUserStore())
	{
		return !SessionValue.IsEmpty();
	}

	if (Persistence == EO3DSecretPersistence::Session)
	{
		UserStore->Remove(CompositeKey);
		return !SessionValue.IsEmpty();
	}

	if (SessionValue.IsEmpty())
	{
		return false;
	}

	UserStore->Save(CompositeKey, SessionValue);
	return true;
}

void FO3DSecretStore::Clear(const FString& Transport, const FString& Profile, const FString& Key)
{
	const FString CompositeKey = MakeCompositeKey(Transport, Profile, Key);
	{
		FScopeLock Lock(&Mutex);
		SessionValues.Remove(CompositeKey);
	}

	if (CanUseUserStore())
	{
		UserStore->Remove(CompositeKey);
	}
}

bool FO3DSecretStore::HasSessionValue(const FString& Transport, const FString& Profile, const FString& Key) const
{
	const FString CompositeKey = MakeCompositeKey(Transport, Profile, Key);
	FScopeLock Lock(&Mutex);
	return SessionValues.Contains(CompositeKey);
}

TOptional<FO3DResolvedSecret> FO3DSecretStore::Resolve(const FString& Transport, const FString& Profile, const FString& Key,
	const FString& EnvVarBaseName) const
{
	if (Key.IsEmpty())
	{
		return TOptional<FO3DResolvedSecret>();
	}

	const FString CompositeKey = MakeCompositeKey(Transport, Profile, Key);

	FO3DResolvedSecret Result;
	{
		FScopeLock Lock(&Mutex);
		if (const FString* Existing = SessionValues.Find(CompositeKey))
		{
			Result.Value = *Existing;
			Result.Source = EO3DSecretSource::Session;
			return TOptional<FO3DResolvedSecret>(MoveTemp(Result));
		}
	}

	if (ResolveEnvironment(EnvVarBaseName, Profile, Result.Value, Result.EnvVarName))
	{
		Result.Source = EO3DSecretSource::Environment;
		return TOptional<FO3DResolvedSecret>(MoveTemp(Result));
	}

	if (LoadRemembered(CompositeKey, Result.Value))
	{
		Result.Source = EO3DSecretSource::UserSettings;
		return TOptional<FO3DResolvedSecret>(MoveTemp(Result));
	}

	return TOptional<FO3DResolvedSecret>();
}

FO3DSecretStatus FO3DSecretStore::Describe(const FString& Transport, const FString& Profile, const FString& Key,
	const FString& EnvVarBaseName) const
{
	FO3DSecretStatus Status;
	const FString CompositeKey = MakeCompositeKey(Transport, Profile, Key);

	FString Ignored;
	Status.bRemembered = LoadRemembered(CompositeKey, Ignored);

	if (TOptional<FO3DResolvedSecret> Resolved = Resolve(Transport, Profile, Key, EnvVarBaseName))
	{
		Status.Source = Resolved->Source;
		Status.EnvVarName = Resolved->EnvVarName;
	}
	return Status;
}

void FO3DSecretStore::ResolveAll(const FString& Transport, const FString& Profile, const TArray<FString>& SecretKeys,
	const TMap<FString, FString>& EnvVarsByKey, TMap<FString, FString>& OutSecrets) const
{
	for (const FString& Key : SecretKeys)
	{
		const FString* EnvVar = EnvVarsByKey.Find(Key);
		if (TOptional<FO3DResolvedSecret> Resolved = Resolve(Transport, Profile, Key, EnvVar ? *EnvVar : FString()))
		{
			OutSecrets.Add(Key, MoveTemp(Resolved->Value));
		}
	}
}
