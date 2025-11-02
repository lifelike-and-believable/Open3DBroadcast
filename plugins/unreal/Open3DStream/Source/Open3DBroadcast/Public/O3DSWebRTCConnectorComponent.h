#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DSWebRTCConnectorComponent.generated.h"

class IWebRTCConnector;

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DBROADCAST_API UO3DSWebRTCConnectorComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UO3DSWebRTCConnectorComponent();

	UPROPERTY(EditAnywhere, Category="WebRTC")
	FString SignalingUrl = TEXT("ws://127.0.0.1:8080");

	// Optional local ID appended to the URL as path segment for libdatachannel sample server compatibility
	UPROPERTY(EditAnywhere, Category="WebRTC")
	FString LocalId;

	// If true, append '/<LocalId>' to SignalingUrl when starting
	UPROPERTY(EditAnywhere, Category="WebRTC")
	bool bAppendLocalIdToUrl = true;

	// If true, act as server (answerer); otherwise client (offerer)
	UPROPERTY(EditAnywhere, Category="WebRTC")
	bool bServer = false;

	// Optional routing id / room used by the signaling server
	UPROPERTY(EditAnywhere, Category="WebRTC")
	FString Room = TEXT("default");

	UPROPERTY(EditAnywhere, Category="WebRTC|Audio")
	bool bEnableAudio = false;

	// Debug: synthesize a sine tone and send as RTP when audio opens (client side)
	UPROPERTY(EditAnywhere, Category="WebRTC|Audio")
	bool bSendDebugTone = false;

	UPROPERTY(EditAnywhere, Category="WebRTC|Audio", meta=(EditCondition="bSendDebugTone"))
	float ToneHz = 440.f;

	UPROPERTY(EditAnywhere, Category="WebRTC|Audio", meta=(EditCondition="bSendDebugTone"))
	float ToneDurationSec = 1.0f;

	UPROPERTY(EditAnywhere, Category="WebRTC")
	bool bVerbose = false;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	TSharedPtr<IWebRTCConnector> Connector;

	void OnState(const FString& State, bool bIsError);
	void OnData(const TArray<uint8>& Bytes);
};
