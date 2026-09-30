// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSenderAudioSinkBase.h"

bool FO3DSenderAudioSinkBase::SubmitPcm(const FString& StreamLabel,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec)
{
	if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
	{
		return false;
	}

	FString EffectiveLabel = StreamLabel;
	if (EffectiveLabel.IsEmpty())
	{
		EffectiveLabel = TEXT("audio_default");
	}

	return OnSubmitPcmInternal(EffectiveLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}

FO3DGatedSenderAudioSink::FO3DGatedSenderAudioSink(FO3DTransportAudioConfig InConfig,
	TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> InGate,
	FO3DSinkAudioEncoder::FSettings InEncoderSettings)
	: FO3DSenderAudioSinkBase(MoveTemp(InConfig))
	, Gate(MoveTemp(InGate))
	, BoundEpoch(Gate->GetEpoch())
	, Encoder(MoveTemp(InEncoderSettings))
{
}

bool FO3DGatedSenderAudioSink::OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec)
{
	FO3DLifetimeGate::FReadScope Scope(*Gate, BoundEpoch);
	if (!Scope)
	{
		return false;
	}

	return OnSubmitGated(ResolvedStreamLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}
