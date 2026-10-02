// Copyright Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DSenderAudioSinkBase.h"

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

FO3DAudioPublishState::FO3DAudioPublishState(TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> InQueue, EO3DAudioWireFormat InWireFormat)
	: Queue(MoveTemp(InQueue))
	, WireFormat(InWireFormat)
{
}

FO3DQueuedSenderAudioSink::FO3DQueuedSenderAudioSink(FO3DAudioPublishStateRef InState,
	FO3DTransportAudioConfig InConfig,
	FO3DSinkAudioEncoder::FSettings InEncoderSettings)
	: FO3DSenderAudioSinkBase(MoveTemp(InConfig))
	, State(MoveTemp(InState))
	, BoundEpoch(State->GetEpoch())
	, Encoder(MoveTemp(InEncoderSettings))
{
}

bool FO3DQueuedSenderAudioSink::MakeAudioBytes(const O3DAudio::FEncodedFrame& Frame, TArray<uint8>& OutBytes) const
{
	if (State->GetWireFormat() == EO3DAudioWireFormat::AudioPayload)
	{
		return O3DAudio::SerializeForTransport(Frame, OutBytes);
	}
	return O3DAudio::CreateUnifiedAudioMessage(Frame, Frame.Meta.TimestampSec, OutBytes);
}

bool FO3DQueuedSenderAudioSink::OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
	const float* Interleaved,
	int32 NumFrames,
	int32 NumChannels,
	int32 SampleRate,
	double TimestampSec)
{
	// WP-S5: held for one encode and the hand-off only; never across a socket or FFI call.
	FO3DLifetimeGate::FReadScope Scope(State->GetGate(), BoundEpoch);
	if (!Scope || !State->IsPeerReady())
	{
		return false;
	}

	// Call-local output: a sink may be fed from more than one thread (TRF-40); the encoder
	// serialises them with its own lock.
	TArray<O3DAudio::FEncodedFrame> Frames;
	if (!Encoder.Encode(ResolvedStreamLabel, State->GetSubjectSlot().Get(), Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Frames))
	{
		return false;
	}

	bool bAllQueued = true;
	for (const O3DAudio::FEncodedFrame& Frame : Frames)
	{
		TArray<uint8> Bytes;
		if (!MakeAudioBytes(Frame, Bytes))
		{
			bAllQueued = false;
			continue;
		}
		const int64 Size = Bytes.Num();
		if (State->GetQueue().Enqueue(FO3DSendItem::MakeAudio(MoveTemp(Bytes), Frame.Meta.SubjectName, Frame.Meta.TimestampSec)) != EO3DSendResult::Queued)
		{
			bAllQueued = false;
			continue;
		}
		State->AddAudioBytesQueued(Size);
	}
	return bAllQueued;
}
