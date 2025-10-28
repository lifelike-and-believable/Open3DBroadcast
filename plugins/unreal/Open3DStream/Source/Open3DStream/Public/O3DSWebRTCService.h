#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "IWebRTCConnector.h"
#include "Open3DStreamSourceSettings.h"
#include "O3DSWebRTCService.generated.h"

UCLASS()
class OPEN3DSTREAM_API UO3DSWebRTCService : public UObject
{
	GENERATED_BODY()
public:
	static UO3DSWebRTCService* Get();
	TSharedPtr<IWebRTCConnector> GetConnector();
	// Register a shared connector so capture/playback components can bind to it
	void SetConnector(const TSharedPtr<IWebRTCConnector>& InConnector);
	void InitializeFromSettings(const FOpen3DStreamSettings& Settings);

private:
	static TWeakObjectPtr<UO3DSWebRTCService> Instance;
	TSharedPtr<IWebRTCConnector> Connector;
};
