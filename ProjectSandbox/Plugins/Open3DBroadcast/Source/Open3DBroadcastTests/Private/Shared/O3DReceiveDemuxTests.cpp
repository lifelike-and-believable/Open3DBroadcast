// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// FO3DUnifiedReceiveDemux (ADR 0007 item 7, WP-A1 step 4; TRB-37, TRB-38, SHR-8, ADR 0011):
// classification of raw and enveloped mocap, audio, control and keepalives; malformed, oversize
// and unknown input; sink ownership.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioFrameCodec.h"
#include "O3DAudioSerialization.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DUnifiedReceiveDemux.h"

namespace O3DReceiveDemuxTests
{
	class FDemuxTestAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& InMeta, const uint8* Data, int32 NumBytes) override
		{
			++Calls;
			Meta = InMeta;
			Pcm = TArray<uint8>(Data, NumBytes);
		}
		int32 Calls = 0;
		O3DS::FAudioFrameMeta Meta;
		TArray<uint8> Pcm;
	};

	class FDemuxTestControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& InStreamId, double /*ReceiveTimeSec*/) override
		{
			Payloads.Emplace(Payload.GetData(), Payload.Num());
			StreamId = InStreamId;
		}
		TArray<TArray<uint8>> Payloads;
		FString StreamId;
	};

	TArray<uint8> MakeFrame(int32 Size, uint8 Seed)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Bytes[Index] = static_cast<uint8>(Seed + Index * 3);
		}
		return Bytes;
	}

	/** An envelope header (24 bytes, little-endian) with any kind, codec and declared payload size, followed by Body. */
	TArray<uint8> MakeRawEnvelope(uint8 Kind, uint8 Codec, uint32 DeclaredPayload, const TArray<uint8>& Body)
	{
		TArray<uint8> Out;
		Out.SetNumZeroed(O3DS::UnifiedWireHeaderSize);
		O3DS::WriteBE32(Out.GetData(), O3DS::FUnifiedHeader::MagicValueBE());
		Out[4] = 2;
		Out[5] = Kind;
		Out[6] = Codec;
		Out[16] = static_cast<uint8>(DeclaredPayload);
		Out[17] = static_cast<uint8>(DeclaredPayload >> 8);
		Out[18] = static_cast<uint8>(DeclaredPayload >> 16);
		Out[19] = static_cast<uint8>(DeclaredPayload >> 24);
		Out.Append(Body);
		return Out;
	}

	O3DAudio::FEncodedFrame MakePcmFrame(int32 SampleRate = 48000)
	{
		O3DAudio::FEncodedFrame Frame;
		Frame.Codec = O3DS::EUnifiedCodec::PCM16;
		Frame.Meta.SourceGuid = FGuid::NewGuid();
		Frame.Meta.StreamLabel = TEXT("voice");
		Frame.Meta.SubjectName = TEXT("Hero");
		Frame.Meta.NumChannels = 1;
		Frame.Meta.SampleRate = SampleRate;
		Frame.Meta.TimestampSec = 1.25;
		Frame.Encoded = MakeFrame(64, 9); // 32 PCM16 samples
		return Frame;
	}

	struct FDemuxFixture
	{
		FDemuxFixture()
			: Consumer(MakeShared<FO3DRecordingFrameConsumer>())
			, Audio(MakeShared<FDemuxTestAudioSink, ESPMode::ThreadSafe>())
			, Control(MakeShared<FDemuxTestControlSink, ESPMode::ThreadSafe>())
		{
			FO3DReceiveDemuxSettings Settings;
			Settings.StreamId = TEXT("stream-1");
			Demux.SetSettings(Settings);
			Demux.SetConsumer(Consumer);
			Demux.SetAudioSink(Audio);
			Demux.SetControlSink(Control);
		}

		EO3DDemuxResult Process(const TArray<uint8>& Bytes, const FString& Subject = FString())
		{
			return Demux.ProcessMessage(Bytes.GetData(), Bytes.Num(), 1.0, Subject);
		}

		FO3DUnifiedReceiveDemux Demux;
		TSharedRef<FO3DRecordingFrameConsumer> Consumer;
		TSharedRef<FDemuxTestAudioSink, ESPMode::ThreadSafe> Audio;
		TSharedRef<FDemuxTestControlSink, ESPMode::ThreadSafe> Control;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiveDemuxMocapTest, "Open3DBroadcast.Shared.ReceiveDemux.Mocap", O3DB_TEST_FLAGS)
bool FO3DReceiveDemuxMocapTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiveDemuxTests;
	FDemuxFixture Fixture;

	const TArray<uint8> Raw = MakeFrame(40, 1);
	TestTrue(TEXT("Raw bytes are legacy mocap"), Fixture.Process(Raw) == EO3DDemuxResult::Mocap);

	const TArray<uint8> Payload = MakeFrame(50, 2);
	TArray<uint8> Envelope;
	TestTrue(TEXT("Envelope built"), O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Mocap, O3DS::EUnifiedCodec::O3DS, Payload.GetData(), Payload.Num(), 2.0, Envelope));
	TestTrue(TEXT("Enveloped mocap"), Fixture.Process(Envelope, TEXT("Hero")) == EO3DDemuxResult::Mocap);

	const TArray<uint8> Direct = MakeFrame(30, 3);
	TestTrue(TEXT("DeliverMocap"), Fixture.Demux.DeliverMocap(TEXT("Villain"), Direct, 3.0) == EO3DDemuxResult::Mocap);

	const TArray<TArray<uint8>> Frames = Fixture.Consumer->GetFrames();
	TestEqual(TEXT("Three frames delivered"), Frames.Num(), 3);
	TestTrue(TEXT("Raw frame byte-exact"), Frames.Num() > 0 && Frames[0] == Raw);
	// TRB-37: the consumer gets the payload inside the envelope, never the header.
	TestTrue(TEXT("Enveloped frame is the payload only"), Frames.Num() > 1 && Frames[1] == Payload);
	TestTrue(TEXT("Direct frame byte-exact"), Frames.Num() > 2 && Frames[2] == Direct);
	TestEqual(TEXT("Mocap counted"), Fixture.Demux.GetStats().Mocap, static_cast<int64>(3));
	TestEqual(TEXT("Mocap bytes counted"), Fixture.Demux.GetStats().MocapBytes, static_cast<int64>(40 + 50 + 30));
	TestEqual(TEXT("Nothing reached the control sink"), Fixture.Control->Payloads.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiveDemuxControlTest, "Open3DBroadcast.Shared.ReceiveDemux.Control", O3DB_TEST_FLAGS)
bool FO3DReceiveDemuxControlTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiveDemuxTests;
	FDemuxFixture Fixture;

	const TArray<uint8> ControlPayload = MakeFrame(100, 7);
	TArray<uint8> Envelope;
	TestTrue(TEXT("Control envelope built"), O3DS::WriteControlEnvelope(ControlPayload, 4.0, Envelope));
	TestTrue(TEXT("Control routed"), Fixture.Process(Envelope) == EO3DDemuxResult::Control);
	TestTrue(TEXT("Control sink got the payload byte-exact"), Fixture.Control->Payloads.Num() == 1 && Fixture.Control->Payloads[0] == ControlPayload);
	TestEqual(TEXT("Control sink got the stream id"), Fixture.Control->StreamId, FString(TEXT("stream-1")));
	TestTrue(TEXT("DeliverControlEnvelope routes too"), Fixture.Demux.DeliverControlEnvelope(Envelope.GetData(), Envelope.Num(), 5.0) == EO3DDemuxResult::Control);

	// ADR 0011: kind Control with the wrong codec, or a payload over the control limit, is dropped, never mocap.
	const TArray<uint8> WrongCodec = MakeRawEnvelope(static_cast<uint8>(O3DS::EUnifiedKind::Control), static_cast<uint8>(O3DS::EUnifiedCodec::O3DS), 10, MakeFrame(10, 1));
	TestTrue(TEXT("Control with the wrong codec is Malformed"), Fixture.Process(WrongCodec) == EO3DDemuxResult::Malformed);
	const TArray<uint8> TooBig = MakeRawEnvelope(static_cast<uint8>(O3DS::EUnifiedKind::Control), static_cast<uint8>(O3DS::EUnifiedCodec::O3DControl),
		O3DS::UnifiedMaxControlPayloadSize + 1, MakeFrame(O3DS::UnifiedMaxControlPayloadSize + 1, 1));
	TestTrue(TEXT("Control over its size limit is Malformed"), Fixture.Process(TooBig) == EO3DDemuxResult::Malformed);
	const TArray<uint8> NotControl = MakeFrame(30, 2);
	TestTrue(TEXT("DeliverControlEnvelope on raw bytes is Malformed"), Fixture.Demux.DeliverControlEnvelope(NotControl.GetData(), NotControl.Num(), 0.0) == EO3DDemuxResult::Malformed);

	TestEqual(TEXT("No control reached the frame consumer"), Fixture.Consumer->Num(), 0);
	TestEqual(TEXT("Two control payloads delivered"), Fixture.Control->Payloads.Num(), 2);
	TestEqual(TEXT("Malformed counted"), Fixture.Demux.GetStats().Malformed, static_cast<int64>(3));

	Fixture.Demux.SetControlSink(nullptr);
	TestTrue(TEXT("Control without a sink is still classified"), Fixture.Process(Envelope) == EO3DDemuxResult::Control);
	TestEqual(TEXT("Counted as undelivered"), Fixture.Demux.GetStats().ControlWithoutSink, static_cast<int64>(1));
	TestEqual(TEXT("And never reaches the consumer"), Fixture.Consumer->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiveDemuxMalformedTest, "Open3DBroadcast.Shared.ReceiveDemux.MalformedOversizeUnknown", O3DB_TEST_FLAGS)
bool FO3DReceiveDemuxMalformedTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiveDemuxTests;
	FDemuxFixture Fixture;

	TestTrue(TEXT("Null buffer"), Fixture.Demux.ProcessMessage(nullptr, 10, 0.0) == EO3DDemuxResult::Malformed);
	const TArray<uint8> Empty;
	TestTrue(TEXT("Empty buffer"), Fixture.Process(Empty) == EO3DDemuxResult::Malformed);

	// The magic with a payload size larger than the buffer: a damaged envelope, never raw mocap.
	const TArray<uint8> Truncated = MakeRawEnvelope(static_cast<uint8>(O3DS::EUnifiedKind::Mocap), 0, 100, MakeFrame(10, 1));
	TestTrue(TEXT("Truncated envelope is Malformed"), Fixture.Process(Truncated) == EO3DDemuxResult::Malformed);
	TArray<uint8> ShortHeader = MakeRawEnvelope(0, 0, 0, TArray<uint8>());
	ShortHeader.SetNum(12);
	TestTrue(TEXT("Magic with a short header is Malformed"), Fixture.Process(ShortHeader) == EO3DDemuxResult::Malformed);

	const TArray<uint8> Unknown = MakeRawEnvelope(7, 0, 4, MakeFrame(4, 1));
	TestTrue(TEXT("Unknown kind is ignored"), Fixture.Process(Unknown) == EO3DDemuxResult::UnknownKind);

	const TArray<uint8> Keepalive = MakeRawEnvelope(static_cast<uint8>(O3DS::EUnifiedKind::Mocap), 0, 0, TArray<uint8>());
	TestTrue(TEXT("An empty payload is a keepalive"), Fixture.Process(Keepalive) == EO3DDemuxResult::Keepalive);

	FO3DReceiveDemuxSettings Settings = Fixture.Demux.GetSettings();
	Settings.MaxMessageBytes = 64;
	Fixture.Demux.SetSettings(Settings);
	TestTrue(TEXT("Over MaxMessageBytes is Oversize"), Fixture.Process(MakeFrame(65, 1)) == EO3DDemuxResult::Oversize);
	TestTrue(TEXT("At MaxMessageBytes is accepted"), Fixture.Process(MakeFrame(64, 1)) == EO3DDemuxResult::Mocap);

	Settings.bAcceptRawMocap = false;
	Fixture.Demux.SetSettings(Settings);
	TestTrue(TEXT("Raw bytes are Malformed when raw mocap is off"), Fixture.Process(MakeFrame(20, 1)) == EO3DDemuxResult::Malformed);

	const FO3DReceiveDemuxStats& Stats = Fixture.Demux.GetStats();
	TestEqual(TEXT("Only the one valid frame reached the consumer"), Fixture.Consumer->Num(), 1);
	TestEqual(TEXT("Malformed"), Stats.Malformed, static_cast<int64>(5));
	TestEqual(TEXT("Oversize"), Stats.Oversize, static_cast<int64>(1));
	TestEqual(TEXT("Unknown kind"), Stats.UnknownKind, static_cast<int64>(1));
	TestEqual(TEXT("Keepalive"), Stats.Keepalive, static_cast<int64>(1));
	TestEqual(TEXT("Rejected total"), Stats.GetRejected(), static_cast<int64>(6));
	TestEqual(TEXT("LexToString"), FString(LexToString(EO3DDemuxResult::Oversize)), FString(TEXT("Oversize")));

	Fixture.Demux.ResetStats();
	TestEqual(TEXT("ResetStats"), Fixture.Demux.GetStats().Malformed, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiveDemuxAudioTest, "Open3DBroadcast.Shared.ReceiveDemux.Audio", O3DB_TEST_FLAGS)
bool FO3DReceiveDemuxAudioTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiveDemuxTests;
	FDemuxFixture Fixture;

	const O3DAudio::FEncodedFrame Frame = MakePcmFrame();
	TArray<uint8> Envelope;
	TestTrue(TEXT("Audio envelope built"), O3DAudio::CreateUnifiedAudioMessage(Frame, Frame.Meta.TimestampSec, Envelope));
	TestTrue(TEXT("Enveloped PCM16 audio"), Fixture.Process(Envelope) == EO3DDemuxResult::Audio);
	TestEqual(TEXT("Sink called once"), Fixture.Audio->Calls, 1);
	TestTrue(TEXT("PCM byte-exact"), Fixture.Audio->Pcm == Frame.Encoded);
	TestEqual(TEXT("Label"), Fixture.Audio->Meta.StreamLabel, Frame.Meta.StreamLabel);
	TestEqual(TEXT("Subject"), Fixture.Audio->Meta.SubjectName, Frame.Meta.SubjectName);
	TestTrue(TEXT("Source"), Fixture.Audio->Meta.SourceGuid == Frame.Meta.SourceGuid);
	TestEqual(TEXT("Rate"), Fixture.Audio->Meta.SampleRate, 48000);

	// A bare audio payload (MoQ's audio track): the codec is read from its header.
	TArray<uint8> Bare;
	TestTrue(TEXT("Bare payload built"), O3DAudio::SerializeForTransport(Frame, Bare));
	O3DS::EUnifiedCodec Peeked = O3DS::EUnifiedCodec::Opus;
	TestTrue(TEXT("Codec peeked"), O3DAudio::TryGetAudioPayloadCodec(Bare.GetData(), Bare.Num(), Peeked) && Peeked == O3DS::EUnifiedCodec::PCM16);
	TestTrue(TEXT("Bare payload delivered"), Fixture.Demux.DeliverAudioPayload(Bare.GetData(), Bare.Num()) == EO3DDemuxResult::Audio);
	TestTrue(TEXT("Parsed frame delivered"), Fixture.Demux.DeliverAudioFrame(Frame.Codec, Frame.Meta, Frame.Encoded.GetData(), Frame.Encoded.Num()) == EO3DDemuxResult::Audio);
	TestEqual(TEXT("Sink called three times"), Fixture.Audio->Calls, 3);

	// SHR-8: metadata out of range on the wire is rejected, not passed on.
	const O3DAudio::FEncodedFrame BadRate = MakePcmFrame(12345);
	TArray<uint8> BadEnvelope;
	TestTrue(TEXT("Envelope with an unsupported rate built"), O3DAudio::CreateUnifiedAudioMessage(BadRate, 0.0, BadEnvelope));
	TestTrue(TEXT("Unsupported rate rejected"), Fixture.Process(BadEnvelope) == EO3DDemuxResult::AudioRejected);
	const TArray<uint8> Garbage = MakeFrame(16, 200);
	TestTrue(TEXT("Garbage bare payload rejected"), Fixture.Demux.DeliverAudioPayload(Garbage.GetData(), Garbage.Num()) == EO3DDemuxResult::AudioRejected);
	const uint8 OddPcm[3] = { 1, 2, 3 };
	TestTrue(TEXT("Odd PCM16 byte count rejected"), Fixture.Demux.DeliverAudioFrame(O3DS::EUnifiedCodec::PCM16, Frame.Meta, OddPcm, 3) == EO3DDemuxResult::AudioRejected);
	TestEqual(TEXT("Rejects never reach the sink"), Fixture.Audio->Calls, 3);
	TestEqual(TEXT("Audio rejects counted"), Fixture.Demux.GetStats().AudioRejected, static_cast<int64>(3));
	TestEqual(TEXT("No audio reached the frame consumer"), Fixture.Consumer->Num(), 0);
	TestEqual(TEXT("PCM16 needs no decoder"), Fixture.Demux.GetNumAudioDecoders(), 0);

	Fixture.Demux.SetAudioSink(nullptr);
	TestTrue(TEXT("Audio without a sink is skipped"), Fixture.Process(Envelope) == EO3DDemuxResult::Audio);
	TestEqual(TEXT("Counted as undelivered"), Fixture.Demux.GetStats().AudioWithoutSink, static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiveDemuxReleaseTest, "Open3DBroadcast.Shared.ReceiveDemux.ReleaseSinks", O3DB_TEST_FLAGS)
bool FO3DReceiveDemuxReleaseTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiveDemuxTests;
	FO3DUnifiedReceiveDemux Demux;
	TWeakPtr<FO3DRecordingFrameConsumer> WeakConsumer;
	TWeakPtr<FDemuxTestAudioSink, ESPMode::ThreadSafe> WeakAudio;
	TWeakPtr<FDemuxTestControlSink, ESPMode::ThreadSafe> WeakControl;
	{
		const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		const TSharedRef<FDemuxTestAudioSink, ESPMode::ThreadSafe> Audio = MakeShared<FDemuxTestAudioSink, ESPMode::ThreadSafe>();
		const TSharedRef<FDemuxTestControlSink, ESPMode::ThreadSafe> Control = MakeShared<FDemuxTestControlSink, ESPMode::ThreadSafe>();
		WeakConsumer = Consumer;
		WeakAudio = Audio;
		WeakControl = Control;
		Demux.SetConsumer(Consumer);
		Demux.SetAudioSink(Audio);
		Demux.SetControlSink(Control);
	}
	TestTrue(TEXT("The demux holds the consumer and sinks strongly while running (TRF-38)"), WeakConsumer.IsValid() && WeakAudio.IsValid() && WeakControl.IsValid());
	TestTrue(TEXT("HasConsumer"), Demux.HasConsumer() && Demux.HasAudioSink() && Demux.HasControlSink());

	Demux.ReleaseSinks();
	TestFalse(TEXT("ReleaseSinks drops the consumer"), WeakConsumer.IsValid());
	TestFalse(TEXT("ReleaseSinks drops the audio sink"), WeakAudio.IsValid());
	TestFalse(TEXT("ReleaseSinks drops the control sink"), WeakControl.IsValid());

	const TArray<uint8> Frame = MakeFrame(10, 1);
	TestTrue(TEXT("Mocap without a consumer is still classified"), Demux.DeliverMocap(TEXT("S"), Frame, 0.0) == EO3DDemuxResult::Mocap);
	TestEqual(TEXT("Counted as undelivered"), Demux.GetStats().MocapWithoutConsumer, static_cast<int64>(1));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
