// Copyright Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DUnifiedReceiveDemux.h"

#include "O3DAudioSerialization.h"

const TCHAR* LexToString(EO3DDemuxResult Result)
{
	switch (Result)
	{
	case EO3DDemuxResult::Mocap: return TEXT("Mocap");
	case EO3DDemuxResult::Audio: return TEXT("Audio");
	case EO3DDemuxResult::Control: return TEXT("Control");
	case EO3DDemuxResult::Keepalive: return TEXT("Keepalive");
	case EO3DDemuxResult::AudioRejected: return TEXT("AudioRejected");
	case EO3DDemuxResult::Malformed: return TEXT("Malformed");
	case EO3DDemuxResult::Oversize: return TEXT("Oversize");
	case EO3DDemuxResult::UnknownKind: return TEXT("UnknownKind");
	default: return TEXT("Unknown");
	}
}

FO3DUnifiedReceiveDemux::FO3DUnifiedReceiveDemux(const FO3DReceiveDemuxSettings& InSettings)
	: Settings(InSettings)
	, AudioDecoder(MakeUnique<O3DAudio::FMultiStreamFrameDecoder>(FMath::Max(1, InSettings.MaxAudioStreams)))
{
}

FO3DUnifiedReceiveDemux::~FO3DUnifiedReceiveDemux() = default;

void FO3DUnifiedReceiveDemux::SetSettings(const FO3DReceiveDemuxSettings& InSettings)
{
	if (InSettings.MaxAudioStreams != Settings.MaxAudioStreams)
	{
		AudioDecoder = MakeUnique<O3DAudio::FMultiStreamFrameDecoder>(FMath::Max(1, InSettings.MaxAudioStreams));
	}
	Settings = InSettings;
}

void FO3DUnifiedReceiveDemux::ReleaseSinks()
{
	Consumer.Reset();
	AudioSink.Reset();
	ControlSink.Reset();
	// Decoder state belongs to the session that ended; a restart decodes from fresh state.
	AudioDecoder->Reset();
}

EO3DDemuxResult FO3DUnifiedReceiveDemux::ProcessMessage(const uint8* Data, int32 Size, double ReceiveTimeSec, const FString& Subject)
{
	if (Data == nullptr || Size <= 0)
	{
		++Stats.Malformed;
		return EO3DDemuxResult::Malformed;
	}
	if (Settings.MaxMessageBytes > 0 && Size > Settings.MaxMessageBytes)
	{
		++Stats.Oversize;
		return EO3DDemuxResult::Oversize;
	}

	O3DS::FUnifiedHeader Header;
	const uint8* PayloadPtr = nullptr;
	int32 PayloadSize = 0;
	if (!O3DS::ParseUnifiedMessage(Data, Size, Header, PayloadPtr, PayloadSize))
	{
		// The envelope magic followed by a header that does not fit the buffer is a damaged
		// envelope, not a legacy raw frame: never hand it to the O3DS parser (T1).
		const bool bEnvelopeMagic = Size >= 4 && O3DS::FUnifiedHeader::ReadBE32(Data) == O3DS::FUnifiedHeader::MagicValueBE();
		if (bEnvelopeMagic || !Settings.bAcceptRawMocap)
		{
			++Stats.Malformed;
			return EO3DDemuxResult::Malformed;
		}
		MocapScratch.Reset();
		MocapScratch.Append(Data, Size);
		return DeliverMocap(Subject, MocapScratch, ReceiveTimeSec);
	}

	if (PayloadSize == 0)
	{
		++Stats.Keepalive;
		return EO3DDemuxResult::Keepalive;
	}

	switch (Header.GetKind())
	{
	case O3DS::EUnifiedKind::Mocap:
		MocapScratch.Reset();
		MocapScratch.Append(PayloadPtr, PayloadSize);
		return DeliverMocap(Subject, MocapScratch, ReceiveTimeSec);
	case O3DS::EUnifiedKind::Audio:
		return DeliverAudioPayload(Header.GetCodec(), PayloadPtr, PayloadSize);
	case O3DS::EUnifiedKind::Control:
		return DeliverControlEnvelope(Data, Size, ReceiveTimeSec);
	default:
		++Stats.UnknownKind;
		return EO3DDemuxResult::UnknownKind;
	}
}

EO3DDemuxResult FO3DUnifiedReceiveDemux::DeliverMocap(const FString& Subject, const TArray<uint8>& Frame, double ReceiveTimeSec)
{
	if (Frame.Num() <= 0)
	{
		++Stats.Malformed;
		return EO3DDemuxResult::Malformed;
	}

	++Stats.Mocap;
	Stats.MocapBytes += Frame.Num();
	if (Consumer.IsValid())
	{
		Consumer->SubmitFrame(Subject.IsEmpty() ? Settings.StreamId : Subject, Frame, ReceiveTimeSec);
	}
	else
	{
		++Stats.MocapWithoutConsumer;
	}
	return EO3DDemuxResult::Mocap;
}

EO3DDemuxResult FO3DUnifiedReceiveDemux::DeliverAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 Size)
{
	if (!AudioSink.IsValid())
	{
		// Nobody listens: skip the parse and the decode.
		++Stats.Audio;
		++Stats.AudioWithoutSink;
		return EO3DDemuxResult::Audio;
	}

	O3DAudio::FEncodedAudioFrame Frame;
	if (!O3DAudio::DeserializeEncodedAudioFrame(Codec, Payload, Size, Frame))
	{
		++Stats.AudioRejected;
		return EO3DDemuxResult::AudioRejected;
	}
	return DeliverAudioFrame(Codec, Frame.Meta, Frame.Payload.GetData(), Frame.Payload.Num());
}

EO3DDemuxResult FO3DUnifiedReceiveDemux::DeliverAudioPayload(const uint8* Payload, int32 Size)
{
	O3DS::EUnifiedCodec Codec = O3DS::EUnifiedCodec::PCM16;
	if (!O3DAudio::TryGetAudioPayloadCodec(Payload, Size, Codec))
	{
		++Stats.AudioRejected;
		return EO3DDemuxResult::AudioRejected;
	}
	return DeliverAudioPayload(Codec, Payload, Size);
}

EO3DDemuxResult FO3DUnifiedReceiveDemux::DeliverAudioFrame(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* Encoded, int32 Size)
{
	if (Encoded == nullptr || Size <= 0)
	{
		++Stats.AudioRejected;
		return EO3DDemuxResult::AudioRejected;
	}
	if (!AudioSink.IsValid())
	{
		++Stats.Audio;
		++Stats.AudioWithoutSink;
		return EO3DDemuxResult::Audio;
	}

	if (Codec == O3DS::EUnifiedCodec::PCM16)
	{
		if (Size % static_cast<int32>(sizeof(int16)) != 0)
		{
			++Stats.AudioRejected;
			return EO3DDemuxResult::AudioRejected;
		}
		++Stats.Audio;
		AudioSink->SubmitPcm16(Meta, Encoded, Size);
		return EO3DDemuxResult::Audio;
	}

	// Opus is stateful: one decoder per (SourceGuid, StreamLabel) (SHR-15).
	if (!AudioDecoder->Decode(Codec, Meta, Encoded, Size, DecodedPcm))
	{
		++Stats.AudioRejected;
		return EO3DDemuxResult::AudioRejected;
	}
	++Stats.Audio;
	if (DecodedPcm.Num() > 0)
	{
		AudioSink->SubmitPcm16(Meta, reinterpret_cast<const uint8*>(DecodedPcm.GetData()), DecodedPcm.Num() * static_cast<int32>(sizeof(int16)));
	}
	return EO3DDemuxResult::Audio;
}

EO3DDemuxResult FO3DUnifiedReceiveDemux::DeliverControlEnvelope(const uint8* Data, int32 Size, double ReceiveTimeSec)
{
	// ADR 0011: the one classifier. Anything that is not a well-formed control envelope is
	// dropped here and never reaches the frame consumer.
	TConstArrayView<uint8> Payload;
	if (!O3DS::TryGetControlPayload(Data, Size, Payload))
	{
		++Stats.Malformed;
		return EO3DDemuxResult::Malformed;
	}

	++Stats.Control;
	if (ControlSink.IsValid())
	{
		ControlSink->SubmitControl(Payload, Settings.StreamId, ReceiveTimeSec);
	}
	else
	{
		++Stats.ControlWithoutSink;
	}
	return EO3DDemuxResult::Control;
}
