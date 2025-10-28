#include "O3DSWebRTCService.h"
#include "Engine/Engine.h"
#include "IWebRTCConnector.h" // Assuming this is the interface for the connector
#include "O3DSWebRTCConnectorFactory.h" // Assuming this is where CreateWebRTCConnector is declared

TWeakObjectPtr<UO3DSWebRTCService> UO3DSWebRTCService::Instance;

UO3DSWebRTCService* UO3DSWebRTCService::Get()
{
	if (!Instance.IsValid())
	{
		Instance = NewObject<UO3DSWebRTCService>(static_cast<UObject*>(GetTransientPackage()));
		Instance->AddToRoot();
	}
	return Instance.Get();
}

TSharedPtr<IWebRTCConnector> UO3DSWebRTCService::GetConnector()
{
	return Connector;
}

void UO3DSWebRTCService::SetConnector(const TSharedPtr<IWebRTCConnector>& InConnector)
{
	Connector = InConnector;
	UE_LOG(LogTemp, Log, TEXT("O3DS WebRTCService: Shared connector set (%p)"), Connector.Get());
}

void UO3DSWebRTCService::InitializeFromSettings(const FOpen3DStreamSettings& Settings)
{
	EO3DSWebRtcBackendReceiver Backend = Settings.WebRtcBackend;
	Connector = CreateWebRTCConnector(Backend, nullptr);
    UE_LOG(LogTemp, Log, TEXT("O3DS WebRTCService: Connector initialized from settings (backend=%d, ptr=%p)"), (int32)Backend, Connector.Get());
}
