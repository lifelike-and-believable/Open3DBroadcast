// Copyright (c) Open3DStream Contributors

// Deprecated forwarding functions (ADR 0007 item 9, WP-A1 PR 1). The sender customization is now
// the sender part (ConfigureSender, SenderOptions) of the descriptor FO3DTransportRegistry keeps
// for each transport name.

#include "O3DSenderTransportCustomization.h"

#include "O3DSenderLegacyTransportShims.h"
#include "O3DSenderComponent.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Templates/UniquePtr.h"

namespace
{
    /**
     * Backs the raw pointers FindTransportCustomization returns. Each item is a copy built from one
     * descriptor; it is dropped when that descriptor is no longer the registered one, so a copy of
     * a transport's functions never outlives the transport's registration (and its module).
     */
    struct FSenderCustomizationCache
    {
        struct FItem
        {
            FO3DTransportDescriptorPtr Source;
            TUniquePtr<FO3DSenderTransportCustomization> Customization;
        };

        FCriticalSection Mutex;
        TMap<FName, FItem> Items;
        FDelegateHandle ChangedHandle;
    };

    FSenderCustomizationCache& GetCache()
    {
        static FSenderCustomizationCache Cache;
        return Cache;
    }

    bool HasSenderPart(const FO3DTransportDescriptor& Descriptor)
    {
        return static_cast<bool>(Descriptor.ConfigureSender) || !Descriptor.SenderOptions.IsEmpty();
    }

    /** The component the current ConfigureSender call is for (FScopedConfiguringComponent). */
    thread_local const UO3DSenderComponent* GConfiguringSenderComponent = nullptr;

    /** Wraps a deprecated component-taking configure function in the WP-A1 PR 5a signature. */
    FO3DSenderConfigureFunction AdaptLegacyConfigure(TFunction<void(const UO3DSenderComponent*, FO3DTransportConfig&)>&& Legacy)
    {
        if (!Legacy)
        {
            return FO3DSenderConfigureFunction();
        }
        return [Legacy = MoveTemp(Legacy)](const FO3DTransportOptionsView& /*Options*/, FO3DTransportConfig& Config)
        {
            Legacy(GConfiguringSenderComponent, Config);
        };
    }

    void PurgeStaleItems()
    {
        FSenderCustomizationCache& Cache = GetCache();
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

O3DSenderLegacyShims::FScopedConfiguringComponent::FScopedConfiguringComponent(const UO3DSenderComponent* Component)
    : Previous(GConfiguringSenderComponent)
{
    GConfiguringSenderComponent = Component;
}

O3DSenderLegacyShims::FScopedConfiguringComponent::~FScopedConfiguringComponent()
{
    GConfiguringSenderComponent = Previous;
}

void O3DSenderLegacyShims::StartCustomizationCache()
{
    FSenderCustomizationCache& Cache = GetCache();
    FScopeLock Lock(&Cache.Mutex);
    if (!Cache.ChangedHandle.IsValid())
    {
        Cache.ChangedHandle = FO3DTransportRegistry::Get().OnTransportsChanged().AddStatic(&PurgeStaleItems);
    }
}

void O3DSenderLegacyShims::StopCustomizationCache()
{
    FSenderCustomizationCache& Cache = GetCache();
    FScopeLock Lock(&Cache.Mutex);
    if (Cache.ChangedHandle.IsValid())
    {
        FO3DTransportRegistry::Get().OnTransportsChanged().Remove(Cache.ChangedHandle);
        Cache.ChangedHandle.Reset();
    }
    Cache.Items.Empty();
}

void O3DSender::RegisterTransportCustomization(FName TransportName, FO3DSenderTransportCustomization&& Customization)
{
    FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [&Customization](FO3DTransportDescriptor& Descriptor)
    {
        Descriptor.ConfigureSender = AdaptLegacyConfigure(MoveTemp(Customization.ConfigureTransport));
        Descriptor.SenderOptions.SecretOptionKeys = MoveTemp(Customization.SecretOptionKeys);
        Descriptor.SenderOptions.SecretEnvVars = MoveTemp(Customization.SecretEnvVars);
        Descriptor.SenderOptions.OptionSchema = MoveTemp(Customization.OptionSchema);
    });
}

void O3DSender::UnregisterTransportCustomization(FName TransportName)
{
    FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [](FO3DTransportDescriptor& Descriptor)
    {
        Descriptor.ConfigureSender = FO3DSenderConfigureFunction();
        Descriptor.SenderOptions = FO3DTransportRoleOptions();
    });
}

const FO3DSenderTransportCustomization* O3DSender::FindTransportCustomization(FName TransportName)
{
    const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(TransportName);
    if (!Descriptor.IsValid() || !HasSenderPart(*Descriptor))
    {
        return nullptr;
    }

    FSenderCustomizationCache& Cache = GetCache();
    FScopeLock Lock(&Cache.Mutex);
    FSenderCustomizationCache::FItem& Item = Cache.Items.FindOrAdd(TransportName);
    if (Item.Source != Descriptor || !Item.Customization.IsValid())
    {
        TUniquePtr<FO3DSenderTransportCustomization> Copy = MakeUnique<FO3DSenderTransportCustomization>();
        // The old signature over the new function: the view is the component's options, and a
        // legacy function behind the adapter gets the component itself.
        if (Descriptor->ConfigureSender)
        {
            Copy->ConfigureTransport = [Configure = Descriptor->ConfigureSender](const UO3DSenderComponent* Component, FO3DTransportConfig& Config)
            {
                const TMap<FString, FString> NoOptions;
                const O3DSenderLegacyShims::FScopedConfiguringComponent Scope(Component);
                Configure(FO3DTransportOptionsView(Component ? Component->TransportOptions : NoOptions), Config);
            };
        }
        Copy->SecretOptionKeys = Descriptor->SenderOptions.SecretOptionKeys;
        Copy->SecretEnvVars = Descriptor->SenderOptions.SecretEnvVars;
        Copy->OptionSchema = Descriptor->SenderOptions.OptionSchema;
        Item.Source = Descriptor;
        Item.Customization = MoveTemp(Copy);
    }
    return Item.Customization.Get();
}

void O3DSender::GetRegisteredTransportNames(TArray<FName>& OutNames)
{
    OutNames = FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender);
}

bool O3DSender::GetTransportSecretDeclaration(FName TransportName, TArray<FString>& OutSecretKeys, TMap<FString, FString>& OutSecretEnvVars)
{
    return FO3DTransportRegistry::Get().GetSecretDeclaration(TransportName, EO3DTransportRole::Sender, OutSecretKeys, OutSecretEnvVars);
}

bool O3DSender::GetTransportOptionSchema(FName TransportName, FO3DTransportOptionSchema& OutSchema)
{
    return FO3DTransportRegistry::Get().GetOptionSchema(TransportName, EO3DTransportRole::Sender, OutSchema);
}
