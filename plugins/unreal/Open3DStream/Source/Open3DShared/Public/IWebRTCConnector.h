// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include <memory>

// Shared backend selection
#include "O3DSWebRtcBackend.h"

/**
 * Backend-agnostic WebRTC connector interface (shared header)
 */
class OPEN3DSHARED_API IWebRTCConnector
{
public:
	virtual ~IWebRTCConnector() = default;

	// Lifecycle
	virtual bool Start(const FString& Url, bool bIsServer) =0;
	virtual void Stop() =0;
	virtual bool IsConnected() const =0;
	virtual void Tick() {}

	// Data
	virtual bool SendDataReliable(const uint8* Data, int32 Size) =0;
	virtual bool SendDataLossy(const uint8* Data, int32 Size) =0;
	virtual void SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback) =0;

	// Audio
	struct FAudioSendConfig
	{
		bool bEnable = false;
		int32 SampleRate =48000;
		int32 NumChannels =1;
		int32 BitrateKbps =64;
		bool bUseDTX = true;

		FString StreamLabel; // o3ds:subject/<Name> or o3ds:mix
		FString TrackLabel; // optional unique id
		FString SubjectName; // for announce/UI
		FString SourceType; // "mic" | "mix"
	};
	virtual bool EnableAudioSend(const FAudioSendConfig&) { return false; }
	virtual bool PushPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) { return false; }

	// Remote audio (subject-aware): StreamLabel, SubjectName, InterleavedPCM, NumFrames, NumChannels, SampleRate
	DECLARE_MULTICAST_DELEGATE_SixParams(FOnRemoteAudio, const FString&, const FString&, const float*, int32, int32, int32);
	virtual FOnRemoteAudio& OnRemoteAudio() =0;

	virtual FString GetLastError() const { return TEXT(""); }
};

struct FLiveKitConfig
{
	FString ServerUrl;
	FString Room;
	FString Token;
	FString Identity;
};

OPEN3DSHARED_API TSharedPtr<IWebRTCConnector> CreateWebRTCConnector(
    EO3DSWebRtcBackend Backend,
    const FLiveKitConfig* LiveKitConfig = nullptr
);
