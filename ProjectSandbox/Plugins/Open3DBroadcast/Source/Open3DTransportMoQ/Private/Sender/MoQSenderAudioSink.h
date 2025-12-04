// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DSenderAudioSinkBase.h"
#include "O3DAudioFrameCodec.h"

class FO3DMoQSender;

/**
 * Audio sink implementation for MoQ sender.
 * 
 * Follows the NNG pattern: captures PCM audio, encodes to PCM16 or Opus,
 * and publishes via a dedicated audio MoQ track.
 * 
 * Threading:
 * - SubmitPcm() may be called from any thread (typically the audio capture thread)
 * - Internal encoder state is protected as needed
 * - Publishes audio through the owning sender's publish mechanism
 */
class FO3DMoQSenderAudioSink final : public FO3DSenderAudioSinkBase
{
public:
	/**
	 * Create an audio sink bound to a MoQ sender.
	 * 
	 * @param InOwner The owning sender instance (must outlive this sink)
	 * @param InAudioConfig Audio configuration (sample rate, channels, codec, etc.)
	 */
	explicit FO3DMoQSenderAudioSink(FO3DMoQSender& InOwner, const FO3DTransportAudioConfig& InAudioConfig);

	virtual ~FO3DMoQSenderAudioSink() = default;

	/** Notification that the capture path has stopped producing frames. */
	virtual void OnCaptureStopped() override;

protected:
	/**
	 * Internal implementation called by base class after validation.
	 * Encodes PCM audio and publishes to the audio track.
	 * 
	 * @param ResolvedStreamLabel Stream label (may be overridden from empty to default)
	 * @param Interleaved Interleaved float PCM samples
	 * @param NumFrames Number of audio frames
	 * @param NumChannels Number of audio channels
	 * @param SampleRate Sample rate in Hz
	 * @param TimestampSec Capture timestamp
	 * @return true if audio was successfully published
	 */
	virtual bool OnSubmitPcmInternal(
		const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override;

private:
	FO3DMoQSender& Owner;
};
