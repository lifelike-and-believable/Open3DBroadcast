// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

class UO3DReceiverSettingsObject;
struct FO3DReceiverSourceConfig;
struct FO3DTransportConfig;
class SO3DTransportConfigPanelBase;

struct FO3DReceiverTransportCustomization
{
    TFunction<void(const FO3DReceiverSourceConfig&, FO3DTransportConfig&)> ConfigureTransport;

#if WITH_EDITOR
    TFunction<TSharedPtr<SO3DTransportConfigPanelBase>(UO3DReceiverSettingsObject*, FSimpleDelegate /*OnSubmit*/)> BuildTransportWidget;
#endif // WITH_EDITOR
};

namespace O3DReceiver
{
    OPEN3DRECEIVER_API void RegisterTransportCustomization(FName TransportName, FO3DReceiverTransportCustomization&& Customization);
    OPEN3DRECEIVER_API void UnregisterTransportCustomization(FName TransportName);
    OPEN3DRECEIVER_API const FO3DReceiverTransportCustomization* FindTransportCustomization(FName TransportName);
    OPEN3DRECEIVER_API void GetRegisteredTransportNames(TArray<FName>& OutNames);
}
