// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "O3DTransportTypes.h"
#include "O3DLifetimeGate.h"
#include "O3DSinkAudioEncoder.h"

/**
 * Lightweight helper base that validates audio submissions and normalises stream labels before
 * forwarding PCM frames to transport-specific implementations.
 */
class OPEN3DSENDER_API FO3DSenderAudioSinkBase : public IO3DSenderAudioSink
{
public:
	explicit FO3DSenderAudioSinkBase(FO3DTransportAudioConfig InConfig)
		: AudioConfig(MoveTemp(InConfig))
	{
	}

	virtual ~FO3DSenderAudioSinkBase() = default;

	virtual bool SubmitPcm(const FString& StreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override final;

protected:
	virtual bool OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) = 0;

	const FO3DTransportAudioConfig& GetAudioConfig() const { return AudioConfig; }

private:
	FO3DTransportAudioConfig AudioConfig;
};

/**
 * Sender audio sink that never references its sender (ADR 0007 addendum, WP-S5).
 *
 * It holds a strong reference to the sender's lifetime gate, the gate epoch that was current
 * when the sink was created, and its own per-label encoders built from an immutable config
 * snapshot. Every submit enters the gate first; after the sender's Stop() has closed the gate,
 * or once a newer session has started, submits return false without touching anything.
 *
 * Subclasses implement OnSubmitGated(), which runs inside the gate and must only use the
 * sink's own members and the transport's shared publish state (never the sender).
 */
class OPEN3DSENDER_API FO3DGatedSenderAudioSink : public FO3DSenderAudioSinkBase
{
public:
	FO3DGatedSenderAudioSink(FO3DTransportAudioConfig InConfig,
		TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> InGate,
		FO3DSinkAudioEncoder::FSettings InEncoderSettings);

	/** Epoch this sink is bound to (0 if the gate was closed when it was created). */
	uint64 GetBoundEpoch() const { return BoundEpoch; }

protected:
	virtual bool OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override final;

	virtual bool OnSubmitGated(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) = 0;

	FO3DSinkAudioEncoder& GetEncoder() { return Encoder; }

private:
	TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate;
	const uint64 BoundEpoch;
	FO3DSinkAudioEncoder Encoder;
};
