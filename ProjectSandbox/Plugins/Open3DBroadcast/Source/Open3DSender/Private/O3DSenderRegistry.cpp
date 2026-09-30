// Copyright Lifelike & Believable. All Rights Reserved.

// Deprecated forwarding functions (ADR 0007 item 9, WP-A1 PR 1). The factories live in the
// descriptor FO3DTransportRegistry keeps for each transport name.

#include "O3DSenderRegistry.h"

#include "O3DSenderLogs.h"

namespace O3DTransport
{
    void RegisterSender(FName TransportName, FO3DSenderFactory&& Factory)
    {
        if (TransportName.IsNone())
        {
            UE_LOG(LogO3DSender, Warning, TEXT("Attempted to register sender factory with None name."));
            return;
        }
        if (!Factory)
        {
            UE_LOG(LogO3DSender, Warning, TEXT("Attempted to register sender factory for '%s' with no callable."), *TransportName.ToString());
            return;
        }

        FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [&Factory](FO3DTransportDescriptor& Descriptor)
        {
            Descriptor.CreateSender = MoveTemp(Factory);
        });
    }

    void UnregisterSender(FName TransportName)
    {
        FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [](FO3DTransportDescriptor& Descriptor)
        {
            Descriptor.CreateSender = FO3DSenderFactory();
        });
    }

    TSharedPtr<IOpen3DSender> CreateSender(FName TransportName)
    {
        return FO3DTransportRegistry::Get().CreateSender(TransportName);
    }

    TArray<FName> GetRegisteredSenders()
    {
        return FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender);
    }
}
