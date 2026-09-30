// Copyright (c) Open3DStream Contributors

#include "O3DReceiverTransportCustomization.h"

#include "O3DReceiverLogs.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSecretStore.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "UObject/Class.h"

namespace
{
    FCriticalSection GReceiverCustomizationMutex;
    TMap<FName, FO3DReceiverTransportCustomization> GReceiverCustomizations;
}

void O3DReceiver::RegisterTransportCustomization(FName TransportName, FO3DReceiverTransportCustomization&& Customization)
{
    FScopeLock Lock(&GReceiverCustomizationMutex);
    GReceiverCustomizations.Add(TransportName, MoveTemp(Customization));
}

void O3DReceiver::UnregisterTransportCustomization(FName TransportName)
{
    FScopeLock Lock(&GReceiverCustomizationMutex);
    GReceiverCustomizations.Remove(TransportName);
}

const FO3DReceiverTransportCustomization* O3DReceiver::FindTransportCustomization(FName TransportName)
{
    FScopeLock Lock(&GReceiverCustomizationMutex);
    return GReceiverCustomizations.Find(TransportName);
}

void O3DReceiver::GetRegisteredTransportNames(TArray<FName>& OutNames)
{
    TArray<FName> LocalNames;
    {
        FScopeLock Lock(&GReceiverCustomizationMutex);
        GReceiverCustomizations.GetKeys(LocalNames);
    }

    LocalNames.Sort(FNameLexicalLess());
    OutNames = MoveTemp(LocalNames);
}

bool O3DReceiver::GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars)
{
    OutSecretKeys.Reset();
    OutSecretEnvVars.Reset();

    FScopeLock Lock(&GReceiverCustomizationMutex);
    const FO3DReceiverTransportCustomization* Customization = GReceiverCustomizations.Find(TransportName);
    if (!Customization)
    {
        return false;
    }

    OutSecretKeys = Customization->SecretOptionKeys;
    OutSecretEnvVars = Customization->SecretEnvVars;
    return true;
}

bool O3DReceiver::IsSecretOptionKey(FName TransportName, const FString& Key)
{
    TArray<FString> SecretKeys;
    TMap<FString, FString> SecretEnvVars;
    GetTransportSecretDeclaration(TransportName, SecretKeys, SecretEnvVars);
    return SecretKeys.Contains(Key);
}

FString O3DReceiver::GetCredentialProfile(const FO3DReceiverSourceConfig& Settings)
{
    const FString ProfileKey = FO3DSecretStore::MakeCredentialProfileOptionKey(Settings.TransportName.ToString());
    const FString* Profile = Settings.TransportOptions.Find(ProfileKey);
    return FO3DSecretStore::NormalizeProfile(Profile ? *Profile : FString());
}

TArray<FString> O3DReceiver::StripSecretOptions(FO3DReceiverSourceConfig& Settings)
{
    TArray<FString> SecretKeys;
    TMap<FString, FString> SecretEnvVars;
    GetTransportSecretDeclaration(Settings.TransportName, SecretKeys, SecretEnvVars);

    TArray<FString> Removed;
    for (const FString& Key : SecretKeys)
    {
        if (Settings.TransportOptions.Remove(Key) > 0)
        {
            Removed.Add(Key);
        }
    }
    return Removed;
}

int32 O3DReceiver::MigrateLegacySecretOptions(FO3DReceiverSourceConfig& Settings, const FString& SourceDescription)
{
    TArray<FString> SecretKeys;
    TMap<FString, FString> SecretEnvVars;
    if (!GetTransportSecretDeclaration(Settings.TransportName, SecretKeys, SecretEnvVars))
    {
        return 0;
    }

    const FString Transport = Settings.TransportName.ToString();
    const FString Profile = GetCredentialProfile(Settings);
    FO3DSecretStore& Store = FO3DSecretStore::Get();

    TArray<FString> Moved;
    for (const FString& Key : SecretKeys)
    {
        FString LegacyValue;
        if (!Settings.TransportOptions.RemoveAndCopyValue(Key, LegacyValue))
        {
            continue;
        }

        Moved.Add(Key);
        // A value the user already set in this session wins over one found in old data.
        if (!LegacyValue.IsEmpty() && !Store.HasSessionValue(Transport, Profile, Key))
        {
            Store.Set(Transport, Profile, Key, LegacyValue, EO3DSecretPersistence::Session);
        }
    }

    if (Moved.Num() > 0)
    {
        // Names the source and the keys, never a value (ADR 0004 item 4).
        UE_LOG(LogO3DReceiver, Warning,
            TEXT("Moved credential option(s) [%s] for transport '%s' out of %s into this session's secret store. ")
            TEXT("Recreate or resave that source so the credential is no longer stored there; it is not rewritten automatically."),
            *FString::Join(Moved, TEXT(", ")), *Transport, *SourceDescription);
    }

    return Moved.Num();
}

FString O3DReceiver::ExportConnectionString(const FO3DReceiverSourceConfig& Settings)
{
    FO3DReceiverSourceConfig Persistable = Settings;
    StripSecretOptions(Persistable);

    FString ConnectionString;
    FO3DReceiverSourceConfig::StaticStruct()->ExportText(ConnectionString, &Persistable, nullptr, nullptr, PPF_None, nullptr);
    return ConnectionString;
}

void O3DReceiver::ResolveSecrets(const FO3DReceiverSourceConfig& Settings, TMap<FString, FString>& OutSecrets)
{
    TArray<FString> SecretKeys;
    TMap<FString, FString> SecretEnvVars;
    if (!GetTransportSecretDeclaration(Settings.TransportName, SecretKeys, SecretEnvVars))
    {
        return;
    }

    FO3DSecretStore::Get().ResolveAll(Settings.TransportName.ToString(), GetCredentialProfile(Settings), SecretKeys, SecretEnvVars, OutSecrets);
}
