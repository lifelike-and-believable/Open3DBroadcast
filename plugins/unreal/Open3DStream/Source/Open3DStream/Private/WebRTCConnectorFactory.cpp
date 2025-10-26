// Copyright (c) Open3DStream Contributors

#include "IWebRTCConnector.h"
#include "LibDataChannelConnector.h"
// #include "LiveKitConnector.h" // TODO: Implement in future PR

TSharedPtr<IWebRTCConnector> CreateWebRTCConnector(
	EO3DSWebRtcBackendReceiver Backend,
	const FLiveKitConfig* LiveKitConfig)
{
	switch (Backend)
	{
	case EO3DSWebRtcBackendReceiver::LibDataChannel:
		{
			TSharedPtr<FLibDataChannelConnector> Connector = MakeShared<FLibDataChannelConnector>();
			return Connector;
		}

	case EO3DSWebRtcBackendReceiver::LiveKit:
		{
			UE_LOG(LogTemp, Error, TEXT("LiveKit connector not yet implemented. Use LibDataChannel backend."));
			// TODO: Implement in future PR after audio track support
			// if (!LiveKitConfig)
			// {
			//     UE_LOG(LogTemp, Error, TEXT("LiveKit backend requires LiveKitConfig"));
			//     return nullptr;
			// }
			// return MakeShared<FLiveKitConnector>(*LiveKitConfig);
			return nullptr;
		}

	default:
		{
			UE_LOG(LogTemp, Error, TEXT("Unknown WebRTC backend type"));
			return nullptr;
		}
	}
}
