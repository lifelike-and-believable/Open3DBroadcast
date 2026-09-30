// Copyright Lifelike & Believable. All Rights Reserved.

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
	// Opus may return zero or several packets per buffer (SHR-2).
	TArray<O3DAudio::FEncodedFrame> Frames;
	if (!GetEncoder().Encode(ResolvedStreamLabel, State->LastSubject.Get(), Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Frames))
	{
		return false;
	}

	bool bAllQueued = true;
	for (const O3DAudio::FEncodedFrame& Frame : Frames)
	{
		TArray<uint8> AudioPayload;
		if (!O3DAudio::SerializeForTransport(Frame, AudioPayload))
		{
			bAllQueued = false;
			continue;
		}

		if (!State->AudioQueue.Enqueue(MoveTemp(AudioPayload)))
		{
			State->AudioDropped.fetch_add(1);
			bAllQueued = false;
		}
	}
	return bAllQueued;
}
