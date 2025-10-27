#include "O3DSWebRTCService.h"
#include "Engine/Engine.h"

TWeakObjectPtr<UO3DSWebRTCService> UO3DSWebRTCService::Instance;

UO3DSWebRTCService* UO3DSWebRTCService::Get()
{
	if (!Instance.IsValid())
	{
		Instance = NewObject<UO3DSWebRTCService>(GetTransientPackage());
		Instance->AddToRoot();
	}
	return Instance.Get();
}

TSharedPtr<IWebRTCConnector> UO3DSWebRTCService::GetConnector()
{
	return Connector;
}

void UO3DSWebRTCService::InitializeFromSettings(const FOpen3DStreamSettings& Settings)
{
	EO3DSWebRtcBackendReceiver Backend = Settings.WebRtcBackend;
	Connector = CreateWebRTCConnector(Backend, nullptr);
}
