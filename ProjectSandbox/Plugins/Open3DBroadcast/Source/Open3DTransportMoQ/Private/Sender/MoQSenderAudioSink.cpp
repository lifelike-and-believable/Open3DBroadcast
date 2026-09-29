// Copyright (c) Open3DStream Contributors

#include "Sender/MoQSenderAudioSink.h"

FO3DMoQSenderAudioSink::FO3DMoQSenderAudioSink(TSharedRef<FMoQSenderAudioState, ESPMode::ThreadSafe> InState, const FO3DTransportAudioConfig& InAudioConfig, FO3DSinkAudioEncoder::FSettings InEncoderSettings)
	: FO3DGatedSenderAudioSink(InAudioConfig, InState->Gate, MoveTemp(InEncoderSettings))
	, State(MoveTemp(InState))
{
}

void FO3DMoQSenderAudioSink::OnCaptureStopped()
{
	// No cleanup needed - audio track is cleaned up when the sender stops
}

bool FO3DMoQSenderAudioSink::OnSubmitGated(
	const FString& ResolvedStreamLabel,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec)
{
	// Runs inside the sender's lifetime gate (WP-S5). Encoding uses this sink's own encoders
	// (TRF-10); the serialized frame goes to the worker through the shared queue.
	O3DAudio::FEncodedFrame Frame;
	if (!GetEncoder().Encode(ResolvedStreamLabel, State->LastSubject.Get(), Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Frame))
	{
		return false;
	}

	TArray<uint8> AudioPayload;
	if (!O3DAudio::SerializeForTransport(Frame, AudioPayload))
	{
		return false;
	}

	if (!State->AudioQueue.Enqueue(MoveTemp(AudioPayload)))
	{
		State->AudioDropped.fetch_add(1);
		return false;
	}
	return true;
}
