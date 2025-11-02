#include "WebRTCConnectorFactory.h"
#include "Logging/LogMacros.h"
#include "LibDataChannelConnector.h"

TSharedPtr<IWebRTCConnector> FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend Backend)
{
    switch (Backend)
    {
    case EO3DSWebRtcBackend::LibDataChannel:
        return MakeShared<FLibDataChannelConnector>();
    case EO3DSWebRtcBackend::LiveKit:
        UE_LOG(LogTemp, Warning, TEXT("WebRTCConnectorFactory: LiveKit backend not implemented (future)"));
        return nullptr;
    default:
        UE_LOG(LogTemp, Warning, TEXT("WebRTCConnectorFactory: Unknown backend (%d)"), (int32)Backend);
        return nullptr;
    }
}
