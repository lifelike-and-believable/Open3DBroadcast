// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DSenderTransportSettings.h"

#include "O3DSenderComponent.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DSenderTransportSettingsPrivate
{
	/** The sender schema's secret keys and their environment variables; false for an unregistered transport. */
	bool GetSecretDeclaration(FName Transport, TArray<FString>& OutKeys, TMap<FString, FString>& OutEnvVars)
	{
		return FO3DTransportRegistry::Get().GetSecretDeclaration(Transport, EO3DTransportRole::Sender, OutKeys, OutEnvVars);
	}
}

bool FO3DSenderTransportSettings::IsSecretKey(FName Transport, const FString& Key)
{
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	O3DSenderTransportSettingsPrivate::GetSecretDeclaration(Transport, SecretKeys, SecretEnvVars);
	return SecretKeys.Contains(Key);
}

FString FO3DSenderTransportSettings::GetOption(const FOptions& Options, FName Transport, const FString& Key)
{
	if (Key.IsEmpty() || IsSecretKey(Transport, Key))
	{
		return FString();
	}
	const FString* Value = Options.Find(Key);
	return Value ? *Value : FString();
}

void FO3DSenderTransportSettings::SetOption(FOptions& Options, FName Transport, const FString& Key, const FString& Value, TFunctionRef<void()> Modify)
{
	if (Key.IsEmpty())
	{
		return;
	}

	// A declared secret goes to the session store, with no Modify(): it must not dirty or enter the asset.
	if (IsSecretKey(Transport, Key))
	{
		SetSecret(Options, Transport, Key, Value, EO3DSecretPersistence::Session, Modify);
		return;
	}

	Modify();
	if (Value.IsEmpty())
	{
		Options.Remove(Key);
	}
	else
	{
		Options.Add(Key, Value);
	}
}

FString FO3DSenderTransportSettings::GetCredentialProfile(const FOptions& Options, FName Transport)
{
	const FString* Profile = Options.Find(FO3DSecretStore::MakeCredentialProfileOptionKey(Transport.ToString()));
	return FO3DSecretStore::NormalizeProfile(Profile ? *Profile : FString());
}

void FO3DSenderTransportSettings::SetSecret(FOptions& Options, FName Transport, const FString& Key, const FString& Value, EO3DSecretPersistence Persistence, TFunctionRef<void()> Modify)
{
	if (Key.IsEmpty())
	{
		return;
	}

	FO3DSecretStore::Get().Set(Transport.ToString(), GetCredentialProfile(Options, Transport), Key, Value, Persistence);

	// A copy left in the map by older data must not be saved again.
	if (Options.Contains(Key))
	{
		Modify();
		Options.Remove(Key);
	}
}

bool FO3DSenderTransportSettings::SetSecretPersistence(const FOptions& Options, FName Transport, const FString& Key, EO3DSecretPersistence Persistence)
{
	return FO3DSecretStore::Get().SetPersistence(Transport.ToString(), GetCredentialProfile(Options, Transport), Key, Persistence);
}

void FO3DSenderTransportSettings::ClearSecret(const FOptions& Options, FName Transport, const FString& Key)
{
	FO3DSecretStore::Get().Clear(Transport.ToString(), GetCredentialProfile(Options, Transport), Key);
}

FO3DSecretStatus FO3DSenderTransportSettings::GetSecretStatus(const FOptions& Options, FName Transport, const FString& Key)
{
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	O3DSenderTransportSettingsPrivate::GetSecretDeclaration(Transport, SecretKeys, SecretEnvVars);
	const FString* EnvVar = SecretEnvVars.Find(Key);
	return FO3DSecretStore::Get().Describe(Transport.ToString(), GetCredentialProfile(Options, Transport), Key, EnvVar ? *EnvVar : FString());
}

TArray<FString> FO3DSenderTransportSettings::MigrateLegacySecrets(FOptions& Options, FName Transport)
{
	TArray<FString> Moved;
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	if (Options.Num() == 0 || !O3DSenderTransportSettingsPrivate::GetSecretDeclaration(Transport, SecretKeys, SecretEnvVars))
	{
		return Moved;
	}

	const FString TransportString = Transport.ToString();
	const FString Profile = GetCredentialProfile(Options, Transport);
	FO3DSecretStore& Store = FO3DSecretStore::Get();
	for (const FString& Key : SecretKeys)
	{
		FString LegacyValue;
		if (!Options.RemoveAndCopyValue(Key, LegacyValue))
		{
			continue;
		}

		Moved.Add(Key);
		// A value the user already set in this session wins over one found in old data.
		if (!LegacyValue.IsEmpty() && !Store.HasSessionValue(TransportString, Profile, Key))
		{
			Store.Set(TransportString, Profile, Key, LegacyValue, EO3DSecretPersistence::Session);
		}
	}
	return Moved;
}

void FO3DSenderTransportSettings::SwitchOptions(FOptions& Active, TMap<FName, FO3DTransportOptionSet>& Inactive, FName From, FName To)
{
	TArray<FString> FromSecretKeys;
	TMap<FString, FString> FromSecretEnvVars;
	const bool bFromRegistered = O3DSenderTransportSettingsPrivate::GetSecretDeclaration(From, FromSecretKeys, FromSecretEnvVars);
	O3DTransportOptions::SwitchTransportOptions(Active, Inactive, From, To, FromSecretKeys, bFromRegistered);
}

void FO3DSenderTransportSettings::BuildConfigOptions(const FOptions& Options, FName Transport, FOptions& OutOptions, FOptions& OutSecrets)
{
	// Declared secret keys are never copied into the options; they are resolved from the secret
	// store (session, environment, per-user settings) into the secrets (ADR 0004).
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	O3DSenderTransportSettingsPrivate::GetSecretDeclaration(Transport, SecretKeys, SecretEnvVars);
	BuildPublicOptions(Options, Transport, OutOptions);
	FO3DSecretStore::Get().ResolveAll(Transport.ToString(), GetCredentialProfile(Options, Transport), SecretKeys, SecretEnvVars, OutSecrets);
}

void FO3DSenderTransportSettings::BuildPublicOptions(const FOptions& Options, FName Transport, FOptions& OutOptions)
{
	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	O3DSenderTransportSettingsPrivate::GetSecretDeclaration(Transport, SecretKeys, SecretEnvVars);
	OutOptions.Reset();
	for (const TPair<FString, FString>& Option : Options)
	{
		if (!SecretKeys.Contains(Option.Key))
		{
			OutOptions.Add(Option.Key, Option.Value);
		}
	}
}

bool FO3DSenderTransportSettings::IsRestartProperty(FName Property)
{
	static const TSet<FName> RestartProps = {
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, CaptureRateHz),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, SubjectName),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TargetMesh),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TargetMeshComponent),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bAutoCreateTransport),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bEnableAudio),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureMode),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioInputDevice),
		GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureConfig)
	};
	return RestartProps.Contains(Property);
}
