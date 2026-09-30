// Copyright (c) Open3DStream Contributors

// Deprecated forwarding functions (ADR 0007 item 9, WP-A1 PR 1). The sender customization is now
// the sender part (ConfigureSender, SenderOptions) of the descriptor FO3DTransportRegistry keeps
// for each transport name.

#include "O3DSenderTransportCustomization.h"

#include "O3DSenderLegacyTransportShims.h"

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
        Descriptor.ConfigureSender = MoveTemp(Customization.ConfigureTransport);
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
        Copy->ConfigureTransport = Descriptor->ConfigureSender;
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
