// Copyright (c) Open3DStream Contributors

#include "O3DReceiverTransportCustomization.h"

#include "O3DReceiverLegacyTransportShims.h"
#include "O3DReceiverLogs.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSecretStore.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Templates/UniquePtr.h"
#include "UObject/Class.h"

// ── Deprecated forwarding functions (ADR 0007 item 9, WP-A1 PR 1) ─────────────────────────
// The receiver customization is now the receiver part (ConfigureReceiver, ReceiverOptions) of
// the descriptor FO3DTransportRegistry keeps for each transport name.

namespace
{
    /**
     * Backs the raw pointers FindTransportCustomization returns. Each item is a copy built from one
     * descriptor; it is dropped when that descriptor is no longer the registered one, so a copy of
     * a transport's functions never outlives the transport's registration (and its module).
     */
    struct FReceiverCustomizationCache
    {
        struct FItem
        {
            FO3DTransportDescriptorPtr Source;
            TUniquePtr<FO3DReceiverTransportCustomization> Customization;
        };

        FCriticalSection Mutex;
        TMap<FName, FItem> Items;
        FDelegateHandle ChangedHandle;
    };

    FReceiverCustomizationCache& GetCache()
    {
        static FReceiverCustomizationCache Cache;
        return Cache;
    }

    bool HasReceiverPart(const FO3DTransportDescriptor& Descriptor)
    {
        return static_cast<bool>(Descriptor.ConfigureReceiver) || !Descriptor.ReceiverOptions.IsEmpty();
    }

    /** The settings the current ConfigureReceiver call is for (FScopedConfiguringSettings). */
    thread_local const FO3DReceiverSourceConfig* GConfiguringReceiverSettings = nullptr;

    /** Wraps a deprecated settings-taking configure function in the WP-A1 PR 5a signature. */
    FO3DReceiverConfigureFunction AdaptLegacyConfigure(TFunction<void(const FO3DReceiverSourceConfig&, FO3DTransportConfig&)>&& Legacy)
    {
        if (!Legacy)
        {
            return FO3DReceiverConfigureFunction();
        }
        return [Legacy = MoveTemp(Legacy)](const FO3DTransportOptionsView& Options, FO3DTransportConfig& Config)
        {
            if (GConfiguringReceiverSettings)
            {
                Legacy(*GConfiguringReceiverSettings, Config);
                return;
            }
            // Called outside a receiver source: settings that carry only the options.
            FO3DReceiverSourceConfig Settings;
            Settings.TransportName = FName(*Config.Transport);
            Settings.TransportOptions = Options.GetValues();
            Legacy(Settings, Config);
        };
    }

    void PurgeStaleItems()
    {
        FReceiverCustomizationCache& Cache = GetCache();
        FScopeLock Lock(&Cache.Mutex);
        for (auto It = Cache.Items.CreateIterator(); It; ++It)
        {
            if (FO3DTransportRegistry::Get().Find(It.Key()) != It.Value().Source)
            {
                It.RemoveCurrent();
            }
        }
    }
}

O3DReceiverLegacyShims::FScopedConfiguringSettings::FScopedConfiguringSettings(const FO3DReceiverSourceConfig& Settings)
    : Previous(GConfiguringReceiverSettings)
{
    GConfiguringReceiverSettings = &Settings;
}

O3DReceiverLegacyShims::FScopedConfiguringSettings::~FScopedConfiguringSettings()
{
    GConfiguringReceiverSettings = Previous;
}

void O3DReceiverLegacyShims::StartCustomizationCache()
{
    FReceiverCustomizationCache& Cache = GetCache();
    FScopeLock Lock(&Cache.Mutex);
    if (!Cache.ChangedHandle.IsValid())
    {
        Cache.ChangedHandle = FO3DTransportRegistry::Get().OnTransportsChanged().AddStatic(&PurgeStaleItems);
    }
}

void O3DReceiverLegacyShims::StopCustomizationCache()
{
    FReceiverCustomizationCache& Cache = GetCache();
    FScopeLock Lock(&Cache.Mutex);
    if (Cache.ChangedHandle.IsValid())
    {
        FO3DTransportRegistry::Get().OnTransportsChanged().Remove(Cache.ChangedHandle);
        Cache.ChangedHandle.Reset();
    }
    Cache.Items.Empty();
}

void O3DReceiver::RegisterTransportCustomization(FName TransportName, FO3DReceiverTransportCustomization&& Customization)
{
    FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [&Customization](FO3DTransportDescriptor& Descriptor)
    {
        Descriptor.ConfigureReceiver = AdaptLegacyConfigure(MoveTemp(Customization.ConfigureTransport));
        Descriptor.ReceiverOptions.SecretOptionKeys = MoveTemp(Customization.SecretOptionKeys);
        Descriptor.ReceiverOptions.SecretEnvVars = MoveTemp(Customization.SecretEnvVars);
        Descriptor.ReceiverOptions.OptionSchema = MoveTemp(Customization.OptionSchema);
    });
}

void O3DReceiver::UnregisterTransportCustomization(FName TransportName)
{
    FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [](FO3DTransportDescriptor& Descriptor)
    {
        Descriptor.ConfigureReceiver = FO3DReceiverConfigureFunction();
        Descriptor.ReceiverOptions = FO3DTransportRoleOptions();
    });
}

const FO3DReceiverTransportCustomization* O3DReceiver::FindTransportCustomization(FName TransportName)
{
    const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(TransportName);
    if (!Descriptor.IsValid() || !HasReceiverPart(*Descriptor))
    {
        return nullptr;
    }

    FReceiverCustomizationCache& Cache = GetCache();
    FScopeLock Lock(&Cache.Mutex);
    FReceiverCustomizationCache::FItem& Item = Cache.Items.FindOrAdd(TransportName);
    if (Item.Source != Descriptor || !Item.Customization.IsValid())
    {
        TUniquePtr<FO3DReceiverTransportCustomization> Copy = MakeUnique<FO3DReceiverTransportCustomization>();
        // The old signature over the new function: the view is the settings' options, and a
        // legacy function behind the adapter gets the settings themselves.
        if (Descriptor->ConfigureReceiver)
        {
            Copy->ConfigureTransport = [Configure = Descriptor->ConfigureReceiver](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
            {
                const O3DReceiverLegacyShims::FScopedConfiguringSettings Scope(Settings);
                Configure(FO3DTransportOptionsView(Settings.TransportOptions), Config);
            };
        }
        Copy->SecretOptionKeys = Descriptor->ReceiverOptions.SecretOptionKeys;
        Copy->SecretEnvVars = Descriptor->ReceiverOptions.SecretEnvVars;
        Copy->OptionSchema = Descriptor->ReceiverOptions.OptionSchema;
        Item.Source = Descriptor;
        Item.Customization = MoveTemp(Copy);
    }
    return Item.Customization.Get();
}

void O3DReceiver::GetRegisteredTransportNames(TArray<FName>& OutNames)
{
    OutNames = FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver);
}

bool O3DReceiver::GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars)
{
    return FO3DTransportRegistry::Get().GetSecretDeclaration(TransportName, EO3DTransportRole::Receiver, OutSecretKeys, OutSecretEnvVars);
}

bool O3DReceiver::GetTransportOptionSchema(FName TransportName, FO3DTransportOptionSchema& OutSchema)
{
    return FO3DTransportRegistry::Get().GetOptionSchema(TransportName, EO3DTransportRole::Receiver, OutSchema);
}

// ── Receiver secret helpers ─────────────────────────────────────────────────────────────

bool O3DReceiver::IsSecretOptionKey(FName TransportName, const FString& Key)
{
    TArray<FString> SecretKeys;
    TMap<FString, FString> SecretEnvVars;
    FO3DTransportRegistry::Get().GetSecretDeclaration(TransportName, EO3DTransportRole::Receiver, SecretKeys, SecretEnvVars);
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
    FO3DTransportRegistry::Get().GetSecretDeclaration(Settings.TransportName, EO3DTransportRole::Receiver, SecretKeys, SecretEnvVars);

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
    if (!FO3DTransportRegistry::Get().GetSecretDeclaration(Settings.TransportName, EO3DTransportRole::Receiver, SecretKeys, SecretEnvVars))
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
    // A source runs one transport; the options put away for others stay in the project settings
    // (SND-35) and are left out of the connection string a LiveLink preset saves.
    Persistable.InactiveTransportOptions.Reset();

    FString ConnectionString;
    FO3DReceiverSourceConfig::StaticStruct()->ExportText(ConnectionString, &Persistable, nullptr, nullptr, PPF_None, nullptr);
    return ConnectionString;
}

void O3DReceiver::ResolveSecrets(const FO3DReceiverSourceConfig& Settings, TMap<FString, FString>& OutSecrets)
{
    TArray<FString> SecretKeys;
    TMap<FString, FString> SecretEnvVars;
    if (!FO3DTransportRegistry::Get().GetSecretDeclaration(Settings.TransportName, EO3DTransportRole::Receiver, SecretKeys, SecretEnvVars))
    {
        return;
    }

    FO3DSecretStore::Get().ResolveAll(Settings.TransportName.ToString(), GetCredentialProfile(Settings), SecretKeys, SecretEnvVars, OutSecrets);
}

bool O3DReceiver::SwitchTransport(FO3DReceiverSourceConfig& Settings, FName NewTransport)
{
    if (Settings.TransportName == NewTransport)
    {
        return false;
    }

    TArray<FString> OutgoingSecretKeys;
    TMap<FString, FString> OutgoingSecretEnvVars;
    const bool bOutgoingRegistered = FO3DTransportRegistry::Get().GetSecretDeclaration(Settings.TransportName, EO3DTransportRole::Receiver, OutgoingSecretKeys, OutgoingSecretEnvVars);

    const FName Outgoing = Settings.TransportName;
    Settings.TransportName = NewTransport;
    O3DTransportOptions::SwitchTransportOptions(Settings.TransportOptions, Settings.InactiveTransportOptions, Outgoing, NewTransport, OutgoingSecretKeys, bOutgoingRegistered);
    return true;
}
