// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "Open3DBroadcastSettings.h"

#include "Misc/ScopeLock.h"
#include "Transport/O3DTransportRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogO3DProjectSettings, Log, All);

namespace
{
	const FO3DTransportDefaultOptions* FindDefaults(FName Transport, EO3DTransportRole Role)
	{
		const UOpen3DBroadcastSettings* Settings = GetDefault<UOpen3DBroadcastSettings>();
		const TMap<FName, FO3DTransportDefaultOptions>& Defaults = Role == EO3DTransportRole::Sender ? Settings->SenderDefaults : Settings->ReceiverDefaults;
		const FO3DTransportDefaultOptions* Found = Defaults.Find(Transport);
		return Found && Found->Options.Num() > 0 ? Found : nullptr;
	}

	/** Once per transport, side and key: the default is in a committed, shipped ini, so it is ignored. */
	void WarnSecretDefaultIgnored(FName Transport, EO3DTransportRole Role, const FString& Key)
	{
		static FCriticalSection WarnedLock;
		static TSet<FString> Warned;
		const TCHAR* Side = Role == EO3DTransportRole::Sender ? TEXT("sender") : TEXT("receiver");
		{
			FScopeLock Lock(&WarnedLock);
			bool bAlreadyWarned = false;
			Warned.Add(FString::Printf(TEXT("%s|%s|%s"), *Transport.ToString(), Side, *Key.ToLower()), &bAlreadyWarned);
			if (bAlreadyWarned)
			{
				return;
			}
		}
		UE_LOG(LogO3DProjectSettings, Warning,
			TEXT("Project Settings > Open3DBroadcast: the %s default for '%s' option '%s' is ignored because the option is a secret (ADR 0004). Remove it from DefaultGame.ini and use the secret store or the option's environment variable."),
			Side, *Transport.ToString(), *Key);
	}
}

UOpen3DBroadcastSettings::UOpen3DBroadcastSettings()
{
	CategoryName = TEXT("Plugins");
}

void UOpen3DBroadcastSettings::ApplyTransportDefaults(FName Transport, EO3DTransportRole Role, TMap<FString, FString>& InOutOptions)
{
	const FO3DTransportDefaultOptions* Defaults = FindDefaults(Transport, Role);
	if (!Defaults)
	{
		return;
	}

	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	if (!FO3DTransportRegistry::Get().GetSecretDeclaration(Transport, Role, SecretKeys, SecretEnvVars))
	{
		return;
	}

	for (const TPair<FString, FString>& Default : Defaults->Options)
	{
		if (SecretKeys.Contains(Default.Key))
		{
			WarnSecretDefaultIgnored(Transport, Role, Default.Key);
			continue;
		}
		if (Default.Value.TrimStartAndEnd().IsEmpty())
		{
			continue;
		}
		// Unset is absent, empty or whitespace, as the options view reads it (O3DTransportOptions.h).
		const FString* Own = InOutOptions.Find(Default.Key);
		if (!Own || Own->TrimStartAndEnd().IsEmpty())
		{
			InOutOptions.Add(Default.Key, Default.Value);
		}
	}
}

void UOpen3DBroadcastSettings::ApplyToSchemaDefaults(FName Transport, EO3DTransportRole Role, FO3DTransportOptionSchema& InOutSchema)
{
	const FO3DTransportDefaultOptions* Defaults = FindDefaults(Transport, Role);
	if (!Defaults)
	{
		return;
	}

	for (FO3DTransportOptionField& Field : InOutSchema)
	{
		if (Field.Type == EO3DTransportOptionType::Secret)
		{
			continue;
		}
		const FString* Default = Defaults->Options.Find(Field.Key);
		if (Default && !Default->TrimStartAndEnd().IsEmpty())
		{
			Field.Default = *Default;
		}
	}
}
