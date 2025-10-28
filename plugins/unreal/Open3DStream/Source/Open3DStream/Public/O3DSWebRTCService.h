#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "IWebRTCConnector.h"
#include "Open3DStreamSourceSettings.h"
#include "O3DSWebRTCService.generated.h"

// Loud logs around connector lifecycle to debug init ordering and availability.
DECLARE_LOG_CATEGORY_EXTERN(LogO3DSWebRTC, Log, All);

UCLASS()
class OPEN3DSTREAM_API UO3DSWebRTCService : public UObject
{
	GENERATED_BODY()
public:
	static UO3DSWebRTCService* Get();
	TSharedPtr<IWebRTCConnector> GetConnector();
	// Register a shared connector so capture/playback components can bind to it
	// Optional Context is appended to logs to identify the call site.
	void SetConnector(const TSharedPtr<IWebRTCConnector>& InConnector, const TCHAR* Context = TEXT(""));
	void InitializeFromSettings(const FOpen3DStreamSettings& Settings);

private:
	static TWeakObjectPtr<UO3DSWebRTCService> Instance;

	// Protect Connector if you plan to set/read from multiple threads; otherwise leave as-is for game-thread use.
	// mutable FCriticalSection ConnectorMutex;

	TSharedPtr<IWebRTCConnector> Connector;

	// Backing storage for the multicast.
	FOnConnectorChanged ConnectorChanged;
};
