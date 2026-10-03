// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DSenderAudioCaptureComponent.h"
#include "Transport/O3DTransportTypes.h"

class AActor;
class IO3DSenderAudioSink;

/** The sender component's audio properties, as one value (WP-A3 step 7). */
struct FO3DSenderAudioSettings
{
	bool bEnableAudio = false;
	EO3DSenderCaptureMode Mode = EO3DSenderCaptureMode::Mix;
	FName InputDevice;
	FName Codec;
	FO3DSenderAudioCaptureConfig CaptureConfig;
};

/**
 * Binds the sender's audio capture to its transport (WP-A3 step 7, SND-22): the capture and
 * transport audio configs built from the properties, finding or creating the capture component,
 * and handing it the transport's sink. The capture component itself stays a property of the
 * sender component. Game thread only.
 */
class FO3DSenderAudioBinding
{
public:
	/** The properties' capture config with the mode's source and, for Input, the cached device index. */
	static FO3DSenderAudioCaptureConfig BuildCaptureConfig(const FO3DSenderAudioSettings& Settings);
	/** What the transport is told about the audio track; only bEnableAudio when audio is off. */
	static FO3DTransportAudioConfig BuildTransportConfig(const FO3DSenderAudioSettings& Settings, const FO3DSenderAudioCaptureConfig& CaptureConfig);
	/** Sets Config's source from the mode and, for Input, its device index (the property kept in step). */
	static void SyncSource(EO3DSenderCaptureMode Mode, FName InputDevice, FO3DSenderAudioCaptureConfig& Config);
	/** The cached index of a capture device name (ADR 0008 item 8); never enumerates. */
	static int32 ResolveDeviceIndex(FName DeviceName);

	/**
	 * Current while it is usable; otherwise Owner's capture component, or a new one registered on
	 * Owner. Null without an owner.
	 */
	static UO3DSenderAudioCaptureComponent* FindOrCreateCaptureComponent(AActor* Owner, UO3DSenderAudioCaptureComponent* Current);
	/** Applies the device and config to Capture and starts it in the settings' mode. */
	static void Configure(UO3DSenderAudioCaptureComponent& Capture, const FO3DSenderAudioSettings& Settings, const FO3DSenderAudioCaptureConfig& CaptureConfig);

	/** Hands Capture the sink (null: none) and stream label; without a sink, logs at most every 2 s. */
	void AttachSink(UO3DSenderAudioCaptureComponent& Capture, const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& Sink,
		const FString& Label, FName TransportName, double NowSeconds);
	/** Takes the sink away from Capture (when set) and resets the warning throttle. */
	void Detach(UO3DSenderAudioCaptureComponent* Capture);

	/** Tests: the time of the last "no sink" warning, 0 when none is pending. */
	double GetLastSinkWarningTime() const { return LastSinkWarningTime; }

private:
	double LastSinkWarningTime = 0.0;
};
