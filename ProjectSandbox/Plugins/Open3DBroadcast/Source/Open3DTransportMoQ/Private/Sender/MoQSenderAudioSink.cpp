// Copyright (c) Open3DStream Contributors

#include "Sender/MoQSenderAudioSink.h"
#include "Sender/MoQSender.h"

FO3DMoQSenderAudioSink::FO3DMoQSenderAudioSink(FO3DMoQSender& InOwner, const FO3DTransportAudioConfig& InAudioConfig)
	: FO3DSenderAudioSinkBase(InAudioConfig)
	, Owner(InOwner)
{
}

void FO3DMoQSenderAudioSink::OnCaptureStopped()
{
	// No cleanup needed - audio track is cleaned up when the sender stops
}

bool FO3DMoQSenderAudioSink::OnSubmitPcmInternal(
	const FString& ResolvedStreamLabel,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec)
{
	return Owner.ProcessCapturedAudio(ResolvedStreamLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}
