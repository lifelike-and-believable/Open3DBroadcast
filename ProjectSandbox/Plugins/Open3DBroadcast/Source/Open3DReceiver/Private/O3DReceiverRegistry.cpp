// Copyright Lifelike & Believable. All Rights Reserved.

// Deprecated forwarding functions (ADR 0007 item 9, WP-A1 PR 1). The factories live in the
// descriptor FO3DTransportRegistry keeps for each transport name.

#include "O3DReceiverRegistry.h"

#include "O3DReceiverLogs.h"

namespace O3DTransport
{
    void RegisterReceiver(FName TransportName, FO3DReceiverFactory&& Factory)
    {
        if (TransportName.IsNone())
        {
            UE_LOG(LogO3DReceiver, Warning, TEXT("Attempted to register receiver factory with None name."));
            return;
        }
        if (!Factory)
        {
            UE_LOG(LogO3DReceiver, Warning, TEXT("Attempted to register receiver factory for '%s' with no callable."), *TransportName.ToString());
            return;
        }

        FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [&Factory](FO3DTransportDescriptor& Descriptor)
        {
            Descriptor.CreateReceiver = MoveTemp(Factory);
        });
    }

    void UnregisterReceiver(FName TransportName)
    {
        FO3DTransportRegistry::Get().EditLegacyDescriptor(TransportName, [](FO3DTransportDescriptor& Descriptor)
        {
            Descriptor.CreateReceiver = FO3DReceiverFactory();
        });
    }

    TSharedPtr<IOpen3DReceiver> CreateReceiver(FName TransportName)
    {
        return FO3DTransportRegistry::Get().CreateReceiver(TransportName);
    }

    TArray<FName> GetRegisteredReceivers()
    {
        return FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver);
    }
}
