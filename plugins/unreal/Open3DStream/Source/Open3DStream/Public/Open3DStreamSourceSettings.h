// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "LiveLinkSourceSettings.h"

#include "Open3DStreamSourceSettings.generated.h"

// Forward declare the enum from Open3DBroadcast module (use int when the module isn't loaded)
// Note: We cannot include the broadcast module header here due to module dependencies,
// so we'll use a plain enum that matches EO3DSWebRtcBackend
UENUM(BlueprintType)
enum class EO3DSWebRtcBackendReceiver : uint8
{
    LibDataChannel UMETA(DisplayName="Peer-to-Peer (libdatachannel)"),
    LiveKit UMETA(DisplayName="LiveKit SFU")
};


USTRUCT(BlueprintType)
struct FOpen3DStreamSettings
{
	GENERATED_BODY()

public:

	FOpen3DStreamSettings() :
		TimeOffset(0.0)
	{}

	UPROPERTY(EditAnywhere, Category="Open3DStream")
	FText Url;

	UPROPERTY(EditAnywhere, Category="Open3DStream", meta=(EditCondition="false", EditConditionHides))
	FText Key;

	UPROPERTY(EditAnywhere, Category="Open3DStream")
	FText Protocol;

	UPROPERTY(EditAnywhere, Category="Open3DStream")
	double  TimeOffset;

	// WebRTC backend selection (only relevant when Protocol is "WebRTC Client" or "WebRTC Server")
	UPROPERTY(EditAnywhere, Category="Open3DStream|WebRTC")
	EO3DSWebRtcBackendReceiver WebRtcBackend = EO3DSWebRtcBackendReceiver::LibDataChannel;

	// Common WebRTC room for both P2P and LiveKit
	UPROPERTY(EditAnywhere, Category="Open3DStream|WebRTC")
	FString WebRtcRoom = TEXT("room1");

	// LiveKit-specific configuration (only used when WebRtcBackend is LiveKit)
	// Connection URL will be taken from Url (wss://). ServerUrl field removed in favor of a single URL field.

	UPROPERTY(EditAnywhere, Category="Open3DStream|WebRTC|LiveKit")
	FString LiveKitToken;

	// Receiver-side: enable native WebRTC audio track playback (when available)
	UPROPERTY(EditAnywhere, Category="Open3DStream|Audio")
	bool bEnableWebRTCAudio = false;

	// Extra playout delay to trade latency for resilience (ms)
	UPROPERTY(EditAnywhere, Category="Open3DStream|Audio", meta=(ClampMin="0", ClampMax="500"))
	int32 WebRTCAudioPlayoutDelayMs = 0;
};

typedef TSharedPtr<FOpen3DStreamSettings, ESPMode::ThreadSafe> FOpen3DStreamSettingsPtr;

UCLASS(Config = GameUserSettings)
class OPEN3DSTREAM_API UOpen3DStreamSettingsObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, GlobalConfig, Category="Open3DStream", Meta=(ShowOnlyInnerProperties))
	FOpen3DStreamSettings Settings;
};

UCLASS()
class OPEN3DSTREAM_API UOpen3DStreamSourceSettings : public ULiveLinkSourceSettings
{
public:
	GENERATED_BODY()

public:
	UOpen3DStreamSourceSettings() {};
};
