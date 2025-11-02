#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "IWebRTCConnector.h"

class OPEN3DSHARED_API FWebRTCConnectorFactory
{
public:
    // Returns a connector for the requested backend, or nullptr if not available.
    static TSharedPtr<IWebRTCConnector> Create(EO3DSWebRtcBackend Backend);
};
