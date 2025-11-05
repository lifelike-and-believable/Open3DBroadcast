#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "IWebRTCConnector.h"

class OPEN3DSHARED_API FWebRTCConnectorFactory
{
public:
    // Returns a connector for the requested backend, or nullptr if not available.
    static TSharedPtr<IWebRTCConnector> Create(EO3DSWebRtcBackend Backend);

    // Backend capability helpers (lightweight, no instance needed by callers)
    static bool BackendSupportsToken(EO3DSWebRtcBackend Backend);
    static const TCHAR* BackendTokenHint(EO3DSWebRtcBackend Backend);
};
