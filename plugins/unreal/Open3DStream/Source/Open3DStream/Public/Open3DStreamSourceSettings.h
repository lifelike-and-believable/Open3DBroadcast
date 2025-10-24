// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "LiveLinkSourceSettings.h"

#include "Open3DStreamSourceSettings.generated.h"


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

	UPROPERTY(EditAnywhere, Category="Open3DStream")
	FText Key;

	UPROPERTY(EditAnywhere, Category="Open3DStream")
	FText Protocol;

	UPROPERTY(EditAnywhere, Category="Open3DStream")
	double  TimeOffset;

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
