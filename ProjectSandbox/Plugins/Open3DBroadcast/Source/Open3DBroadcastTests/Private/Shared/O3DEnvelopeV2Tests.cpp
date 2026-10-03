// Copyright Lifelike & Believable. All Rights Reserved.

// Envelope v2 (ADR 0009 item 4, WP-A4): writers emit "O3DU", little-endian, with a sequence
// number; readers still accept envelope v1 ("O3DA", big-endian). The codec is the core's
// (o3ds/wire_format.h); these check the plugin's wrappers and writers.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioFrameCodec.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DTransportTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DEnvelopeV2WrittenAndV1ReadTest, "Open3DBroadcast.Shared.Envelope.V2WrittenV1StillRead", O3DB_TEST_FLAGS)
bool FO3DEnvelopeV2WrittenAndV1ReadTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Payload = { 10, 20, 30 };

	TArray<uint8> Message;
	if (!TestTrue(TEXT("Created"), O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Audio, O3DS::EUnifiedCodec::PCM16, Payload.GetData(), Payload.Num(), 1.5, Message, 7)))
	{
		return false;
	}
	TestEqual(TEXT("v2 header plus payload"), Message.Num(), O3DS::UnifiedWireHeaderSize + Payload.Num());
	TestTrue(TEXT("Magic O3DU, version 2"), Message[0] == 'O' && Message[1] == '3' && Message[2] == 'D' && Message[3] == 'U' && Message[4] == 2);
	TestTrue(TEXT("Timestamp little-endian (1,500,000 us)"), Message[8] == 0x60 && Message[9] == 0xE3 && Message[10] == 0x16 && Message[11] == 0x00);
	TestTrue(TEXT("Sequence little-endian"), Message[20] == 7 && Message[21] == 0 && Message[22] == 0 && Message[23] == 0);
	TestTrue(TEXT("Envelope magic recognised"), O3DS::HasUnifiedEnvelopeMagic(Message.GetData(), Message.Num()));

	O3DS::FUnifiedHeader Header;
	const uint8* PayloadPtr = nullptr;
	int32 PayloadSize = 0;
	TestTrue(TEXT("Parses"), O3DS::ParseUnifiedMessage(Message.GetData(), Message.Num(), Header, PayloadPtr, PayloadSize));
	TestEqual(TEXT("Version"), static_cast<int32>(Header.Version), 2);
	TestEqual(TEXT("Sequence"), Header.Seq, static_cast<uint32>(7));
	TestEqual(TEXT("Header size"), Header.HeaderSize, O3DS::UnifiedWireHeaderSize);
	TestEqual(TEXT("Timestamp"), Header.TimestampUs(), static_cast<uint64>(1500000));
	TestTrue(TEXT("Payload view"), PayloadPtr == Message.GetData() + O3DS::UnifiedWireHeaderSize && PayloadSize == Payload.Num());

	// Envelope v1, as a pre-D8 sender wrote it: still read.
	TArray<uint8> V1;
	V1.SetNumZeroed(O3DS::UnifiedWireHeaderSizeV1 + Payload.Num());
	O3DS::WriteBE32(V1.GetData(), O3DS::FUnifiedHeader::MagicValueBE());
	V1[4] = 1;
	V1[5] = static_cast<uint8>(O3DS::EUnifiedKind::Audio);
	V1[6] = static_cast<uint8>(O3DS::EUnifiedCodec::PCM16);
	O3DS::WriteBE64(V1.GetData() + 8, 2000000);
	O3DS::WriteBE32(V1.GetData() + 16, static_cast<uint32>(Payload.Num()));
	FMemory::Memcpy(V1.GetData() + O3DS::UnifiedWireHeaderSizeV1, Payload.GetData(), Payload.Num());
	TestTrue(TEXT("v1 parses"), O3DS::ParseUnifiedMessage(V1.GetData(), V1.Num(), Header, PayloadPtr, PayloadSize));
	TestEqual(TEXT("v1 version"), static_cast<int32>(Header.Version), 1);
	TestEqual(TEXT("v1 header size"), Header.HeaderSize, O3DS::UnifiedWireHeaderSizeV1);
	TestEqual(TEXT("v1 timestamp"), Header.TimestampUs(), static_cast<uint64>(2000000));
	TestTrue(TEXT("v1 payload view"), PayloadPtr == V1.GetData() + O3DS::UnifiedWireHeaderSizeV1 && PayloadSize == Payload.Num());

	// Control: a v2 envelope at the budget's limit is still 1,100 bytes (ADR 0011 item 4).
	TArray<uint8> MaxControl;
	MaxControl.SetNumZeroed(O3DS::UnifiedMaxControlPayloadSize);
	TArray<uint8> Envelope;
	TestTrue(TEXT("Largest control envelope written"), O3DS::WriteControlEnvelope(MaxControl, 1.0, Envelope, 3));
	TestEqual(TEXT("Within the 1,100-byte budget"), Envelope.Num(), 1100);
	TConstArrayView<uint8> ControlPayload;
	TestTrue(TEXT("Control payload found"), O3DS::TryGetControlPayload(Envelope.GetData(), Envelope.Num(), ControlPayload));
	TestEqual(TEXT("Control payload size"), ControlPayload.Num(), O3DS::UnifiedMaxControlPayloadSize);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DEnvelopeAudioSequenceTest, "Open3DBroadcast.Shared.Envelope.AudioFramesAreNumbered", O3DB_TEST_FLAGS)
bool FO3DEnvelopeAudioSequenceTest::RunTest(const FString& Parameters)
{
	FO3DTransportAudioConfig Config;
	Config.bEnableAudio = true;
	Config.Codec = TEXT("pcm16");
	O3DAudio::FFrameEncoder Encoder;
	if (!TestTrue(TEXT("Encoder initialised"), Encoder.Initialize(Config, TEXT("Stage"), TEXT("Hero"))))
	{
		return false;
	}

	TArray<float> Samples;
	Samples.Init(0.25f, 480);
	TArray<O3DAudio::FEncodedFrame> Frames;
	for (int32 Buffer = 0; Buffer < 3; ++Buffer)
	{
		TestTrue(TEXT("Encodes"), Encoder.EncodeBuffer(FString(), FString(), Samples.GetData(), Samples.Num(), 1, 48000, 1.0 + Buffer * 0.01, Frames));
	}
	if (!TestEqual(TEXT("Three PCM16 frames"), Frames.Num(), 3))
	{
		return false;
	}
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		TArray<uint8> Message;
		TestTrue(TEXT("Envelope written"), O3DAudio::CreateUnifiedAudioMessage(Frames[Index], Frames[Index].Meta.TimestampSec, Message));
		O3DS::FUnifiedHeader Header;
		const uint8* PayloadPtr = nullptr;
		int32 PayloadSize = 0;
		TestTrue(TEXT("Parses"), O3DS::ParseUnifiedMessage(Message.GetData(), Message.Num(), Header, PayloadPtr, PayloadSize));
		TestEqual(TEXT("Frames are numbered in order"), Header.Seq, static_cast<uint32>(Index));
	}

	// PCM16 is little-endian on the wire: 0.25 is 8192 (0x2000), low byte first.
	const TArray<uint8>& Pcm = Frames[0].Encoded;
	TestTrue(TEXT("Little-endian samples"), Pcm.Num() >= 2 && Pcm[0] == 0x00 && Pcm[1] == 0x20);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
