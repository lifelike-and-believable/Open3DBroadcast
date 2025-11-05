#include "WebRTCConnectorFactory.h"
#include "Logging/LogMacros.h"
#include "LibDataChannelConnector.h"
#include "LiveKitConnector.h"

TSharedPtr<IWebRTCConnector> FWebRTCConnectorFactory::Create(EO3DSWebRtcBackend Backend)
{
    switch (Backend)
    {
    case EO3DSWebRtcBackend::LibDataChannel:
        return MakeShared<FLibDataChannelConnector>();
    case EO3DSWebRtcBackend::LiveKit:
        return MakeShared<FLiveKitConnector>();
    default:
        UE_LOG(LogTemp, Warning, TEXT("WebRTCConnectorFactory: Unknown backend (%d)"), (int32)Backend);
        return nullptr;
    }
}

bool FWebRTCConnectorFactory::BackendSupportsToken(EO3DSWebRtcBackend Backend)
{
    switch (Backend)
    {
    case EO3DSWebRtcBackend::LibDataChannel: return false;
    case EO3DSWebRtcBackend::LiveKit:       return true;
    default:                                return false;
    }
}

const TCHAR* FWebRTCConnectorFactory::BackendTokenHint(EO3DSWebRtcBackend Backend)
{
    switch (Backend)
    {
    case EO3DSWebRtcBackend::LibDataChannel: return TEXT("unused");
    case EO3DSWebRtcBackend::LiveKit:        return TEXT("client token");
    default:                                 return TEXT("unused");
    }
}
