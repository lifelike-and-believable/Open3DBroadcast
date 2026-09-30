// Copyright (c) Open3DStream Contributors
//
// WP-S9 (ADR 0004): the sender component never saves a secret transport option. A secret set
// through SetTransportOption goes to FO3DSecretStore, is resolved into FO3DTransportConfig::Secrets,
// and appears in none of the serialized component bytes or the saved package file. A legacy
// secret found in TransportOptions is migrated to the session store with a Warning that names the
// asset but not the value.
//
// Uses a test-only transport name and key, so no transport module (and no LiveKit key name) is
// involved. No network. Self-contained so it can move to the Open3DBroadcastTests module (WP-T2).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Package.h"
#if WITH_EDITOR
#include "UObject/SavePackage.h"
#endif

#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "O3DSenderTransportCustomization.h"

/** White-box access to UO3DSenderComponent for the WP-S9 tests (befriended in O3DSenderComponent.h). */
struct FO3DSenderSecretsTestAccess
{
	static FO3DTransportConfig BuildTransportConfig(const UO3DSenderComponent& Component)
	{
		return Component.BuildTransportConfig();
	}

	static int32 MigrateLegacySecretOptions(UO3DSenderComponent& Component)
	{
		return Component.MigrateLegacySecretOptions();
	}
};

namespace O3DSenderSecretsTestUtil
{
	static const TCHAR* const TransportName = TEXT("O3DSecretsTestSender");
	static const TCHAR* const SecretKey = TEXT("o3dsecretstestsender.token");
	static const TCHAR* const UrlKey = TEXT("o3dsecretstestsender.url");
	static const TCHAR* const EnvVar = TEXT("O3DB_SECRETSTEST_SENDER_TOKEN");

	/** Registers a sender customization declaring one secret key; unregisters and clears the store on exit. */
	struct FScopedSecretsTestTransport
	{
		FScopedSecretsTestTransport()
		{
			FO3DSenderTransportCustomization Customization;
			Customization.SecretOptionKeys.Add(SecretKey);
			Customization.SecretEnvVars.Add(SecretKey, EnvVar);
			O3DSender::RegisterTransportCustomization(TransportName, MoveTemp(Customization));
			ClearStore();
		}

		~FScopedSecretsTestTransport()
		{
			ClearStore();
			O3DSender::UnregisterTransportCustomization(TransportName);
		}

		static void ClearStore()
		{
			FO3DSecretStore::Get().Clear(TransportName, TEXT("default"), SecretKey);
			FO3DSecretStore::Get().Clear(TransportName, TEXT("stage"), SecretKey);
		}
	};

	/** Captures every log line while alive, so a test can assert a value was never logged. */
	class FSecretsTestLogCapture final : public FOutputDevice
	{
	public:
		FSecretsTestLogCapture()
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FSecretsTestLogCapture() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			FScopeLock Lock(&Mutex);
			Lines.Add(FString(V));
		}

		virtual bool CanBeUsedOnAnyThread() const override
		{
			return true;
		}

		bool AnyLineContains(const FString& Needle)
		{
			GLog->Flush();
			FScopeLock Lock(&Mutex);
			for (const FString& Line : Lines)
			{
				if (Line.Contains(Needle, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

	private:
		FCriticalSection Mutex;
		TArray<FString> Lines;
	};

	static bool ContainsSequence(const TArray<uint8>& Haystack, const TArray<uint8>& Needle)
	{
		if (Needle.Num() == 0 || Haystack.Num() < Needle.Num())
		{
			return false;
		}
		for (int32 Start = 0; Start + Needle.Num() <= Haystack.Num(); ++Start)
		{
			if (FMemory::Memcmp(Haystack.GetData() + Start, Needle.GetData(), Needle.Num()) == 0)
			{
				return true;
			}
		}
		return false;
	}

	/** True when Bytes holds Text as ANSI/UTF-8 or as UTF-16LE (the two ways FString serializes). */
	static bool BytesContainString(const TArray<uint8>& Bytes, const FString& Text)
	{
		TArray<uint8> Narrow;
		TArray<uint8> Wide;
		for (const TCHAR C : Text)
		{
			Narrow.Add(static_cast<uint8>(C & 0xFF));
			Wide.Add(static_cast<uint8>(C & 0xFF));
			Wide.Add(static_cast<uint8>((C >> 8) & 0xFF));
		}
		return ContainsSequence(Bytes, Narrow) || ContainsSequence(Bytes, Wide);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderSecretsRoutingTest, "Open3DBroadcast.Sender.Secrets.SetTransportOptionRoutesToStore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderSecretsRoutingTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderSecretsTestUtil;
	FScopedSecretsTestTransport ScopedTransport;

	const FString Token = TEXT("SENDER-ROUTING-TOKEN-7f3a9c");
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->SetTransportName(TransportName);

	Component->SetTransportOption(UrlKey, TEXT("wss://example.invalid"));
	Component->SetTransportOption(SecretKey, Token);

	TestTrue(TEXT("Secret key is declared"), Component->IsTransportSecretKey(SecretKey));
	TestFalse(TEXT("Secret not in TransportOptions"), Component->TransportOptions.Contains(SecretKey));
	TestTrue(TEXT("Plain option stored"), Component->TransportOptions.Contains(UrlKey));
	TestTrue(TEXT("GetTransportOption never returns a secret"), Component->GetTransportOption(SecretKey).IsEmpty());
	TestTrue(TEXT("Status: set for this session"), Component->GetTransportSecretStatus(SecretKey).Source == EO3DSecretSource::Session);

	FO3DTransportConfig Config = FO3DSenderSecretsTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("Secret resolved into Config.Secrets"), Config.Secrets.FindRef(SecretKey), Token);
	TestFalse(TEXT("Secret not copied into AdvancedParams"), Config.AdvancedParams.Contains(SecretKey));
	TestTrue(TEXT("Plain option copied into AdvancedParams"), Config.AdvancedParams.Contains(UrlKey));
	TestFalse(TEXT("ToDebugString does not show the secret"), Config.ToDebugString().Contains(Token));

	// A different credential profile does not see the default profile's secret.
	Component->SetTransportOption(FO3DSecretStore::MakeCredentialProfileOptionKey(TransportName), TEXT("stage"));
	TestEqual(TEXT("Profile option is an ordinary option"), Component->GetCredentialProfile(), FString(TEXT("stage")));
	Config = FO3DSenderSecretsTestAccess::BuildTransportConfig(*Component);
	TestFalse(TEXT("Other profile: nothing resolves"), Config.Secrets.Contains(SecretKey));

	const FString StageToken = TEXT("SENDER-STAGE-TOKEN-51d0");
	Component->SetTransportSecret(SecretKey, StageToken);
	Config = FO3DSenderSecretsTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("Stage profile resolves its own secret"), Config.Secrets.FindRef(SecretKey), StageToken);

	// An empty value clears the secret.
	Component->SetTransportOption(SecretKey, FString());
	TestTrue(TEXT("Cleared: status none"), Component->GetTransportSecretStatus(SecretKey).Source != EO3DSecretSource::Session);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderSecretsSavedAssetTest, "Open3DBroadcast.Sender.Secrets.SavedComponentHasNoToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderSecretsSavedAssetTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderSecretsTestUtil;
	FScopedSecretsTestTransport ScopedTransport;

	const FString Token = TEXT("SENDER-SAVED-TOKEN-c41e0b");
	const FString PackageName = FString::Printf(TEXT("/Temp/O3DBSecretsTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UPackage* Package = CreatePackage(*PackageName);
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(Package, TEXT("SecretsTestSender"), RF_Public | RF_Standalone);
	Component->SetTransportName(TransportName);
	Component->SetTransportOption(UrlKey, TEXT("wss://example.invalid"));
	Component->SetTransportOption(SecretKey, Token);

	// 1. Tagged-property serialization of the component (what a level or Blueprint stores for it).
	TArray<uint8> ObjectBytes;
	{
		FObjectWriter Writer(Component, ObjectBytes, /*bIgnoreClassRef=*/false, /*bIgnoreArchetypeRef=*/false, /*bDoDelta=*/false);
	}
	TestTrue(TEXT("Serialized component is not empty"), ObjectBytes.Num() > 0);
	TestTrue(TEXT("Sanity: a plain option value is serialized"), BytesContainString(ObjectBytes, TEXT("wss://example.invalid")));
	TestFalse(TEXT("Token absent from the serialized component"), BytesContainString(ObjectBytes, Token));

#if WITH_EDITOR
	// 2. The package saved to disk (package saving is editor-only).
	const FString Filename = FPaths::Combine(FPaths::AutomationTransientDir(),
		FString::Printf(TEXT("O3DBSecretsTest_%s.uasset"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, Component, *Filename, SaveArgs);
	TestTrue(TEXT("Package saved"), bSaved);
	if (bSaved)
	{
		TArray<uint8> FileBytes;
		TestTrue(TEXT("Saved package read back"), FFileHelper::LoadFileToArray(FileBytes, *Filename));
		TestTrue(TEXT("Sanity: a plain option value is in the file"), BytesContainString(FileBytes, TEXT("wss://example.invalid")));
		TestFalse(TEXT("Token absent from the saved package"), BytesContainString(FileBytes, Token));
		IFileManager::Get().Delete(*Filename, /*RequireExists=*/false, /*EvenReadOnly=*/true);
	}
#endif // WITH_EDITOR

	Component->ClearFlags(RF_Public | RF_Standalone);
	Component->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderSecretsMigrationTest, "Open3DBroadcast.Sender.Secrets.MigratesLegacyToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DSenderSecretsMigrationTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderSecretsTestUtil;
	FScopedSecretsTestTransport ScopedTransport;

	const FString LegacyToken = TEXT("SENDER-LEGACY-TOKEN-99ab21");
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->SetTransportName(TransportName);

	// Emulate a component loaded from an asset saved before WP-S9.
	Component->TransportOptions.Add(SecretKey, LegacyToken);
	Component->TransportOptions.Add(UrlKey, TEXT("wss://example.invalid"));

	// Two migrations below each log one Warning.
	AddExpectedError(TEXT("Moved credential option"), EAutomationExpectedMessageFlags::Contains, 2);

	int32 Moved = 0;
	{
		FSecretsTestLogCapture Capture;
		Moved = FO3DSenderSecretsTestAccess::MigrateLegacySecretOptions(*Component);
		TestFalse(TEXT("The Warning does not contain the value"), Capture.AnyLineContains(LegacyToken));
		TestTrue(TEXT("The Warning names the key"), Capture.AnyLineContains(SecretKey));
		TestTrue(TEXT("The Warning names the asset"), Capture.AnyLineContains(Component->GetPackage()->GetName()));
	}

	TestEqual(TEXT("One key migrated"), Moved, 1);
	TestFalse(TEXT("Removed from TransportOptions"), Component->TransportOptions.Contains(SecretKey));
	TestTrue(TEXT("Other options kept"), Component->TransportOptions.Contains(UrlKey));

	TOptional<FO3DResolvedSecret> Resolved = FO3DSecretStore::Get().Resolve(TransportName, TEXT("default"), SecretKey);
	TestTrue(TEXT("Moved into the session store"), Resolved.IsSet() && Resolved->Source == EO3DSecretSource::Session);
	TestTrue(TEXT("Value preserved"), Resolved.IsSet() && Resolved->Value == LegacyToken);

	TestEqual(TEXT("Second run migrates nothing"), FO3DSenderSecretsTestAccess::MigrateLegacySecretOptions(*Component), 0);

	// A value the user already set in this session is not replaced by older data.
	const FString SessionToken = TEXT("SENDER-SESSION-TOKEN-0c7d");
	Component->SetTransportSecret(SecretKey, SessionToken);
	Component->TransportOptions.Add(SecretKey, LegacyToken);
	TestEqual(TEXT("Legacy copy removed again"), FO3DSenderSecretsTestAccess::MigrateLegacySecretOptions(*Component), 1);
	Resolved = FO3DSecretStore::Get().Resolve(TransportName, TEXT("default"), SecretKey);
	TestTrue(TEXT("Session value kept"), Resolved.IsSet() && Resolved->Value == SessionToken);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
