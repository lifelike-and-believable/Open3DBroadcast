// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DSenderAudioSinkBase.h"
#include "O3DAudioFrameCodec.h"
#include "O3DEncodedPayloadQueue.h"

#include <atomic>

/**
 * Publish state shared between FO3DMoQSender, its worker and its audio sinks
 * (ADR 0007 addendum, WP-S5: TRF-1, TRF-10). Holds no sender pointer and no FFI handle.
 */
struct FMoQSenderAudioState
{
	TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();
	/** Serialized audio frames for the worker. Its wake event is also the worker's wake event. */
	FO3DEncodedPayloadQueue AudioQueue{1024 * 1024};
	FO3DAudioSubjectSlot LastSubject;
	std::atomic<int64> AudioDropped{0};
};

/**
 * Audio sink implementation for MoQ sender.
 *
 * Captures PCM audio, encodes to PCM16 or Opus with its own per-label encoders, and hands
 * the serialized frame to the sender's worker, which publishes it on the audio MoQ track.
 *
 * Threading:
 * - SubmitPcm() may be called from any thread (typically the audio capture thread)
 * - It never references the sender; after the sender's Stop() it returns false
 */
class FO3DMoQSenderAudioSink final : public FO3DGatedSenderAudioSink
{
public:
	/**
	 * Create an audio sink bound to a MoQ sender's shared audio state.
	 *
	 * @param InState Shared publish state (the sink keeps it alive; it holds no sender reference)
	 * @param InAudioConfig Audio configuration (sample rate, channels, codec, etc.)
	 * @param InEncoderSettings Immutable snapshot for this sink's encoders
	 */
	FO3DMoQSenderAudioSink(TSharedRef<FMoQSenderAudioState, ESPMode::ThreadSafe> InState, const FO3DTransportAudioConfig& InAudioConfig, FO3DSinkAudioEncoder::FSettings InEncoderSettings);

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
	virtual bool OnSubmitGated(
		const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override;

private:
	TSharedRef<FMoQSenderAudioState, ESPMode::ThreadSafe> State;
};
