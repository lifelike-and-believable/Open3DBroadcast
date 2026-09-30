// Copyright (c) Open3DStream Contributors
//
// SHR-6 (WP-T2): direct tests for the Shared parsers that read untrusted network bytes: the
// unified message envelope (O3DUnifiedMessage.h) and the audio frame format
// (O3DAudioSerialization.h). Every truncation of a valid buffer must be rejected, and a round
// trip must keep every field.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DAudioSerialization.h"
#include "O3DUnifiedMessage.h"

namespace O3DSharedParserTests
{
	O3DS::FAudioFrameMeta MakeMeta()
	{
		O3DS::FAudioFrameMeta Meta;
		Meta.SourceGuid = FGuid(1, 2, 3, 4);
		Meta.StreamLabel = TEXT("o3ds:mix/hero");
		Meta.SubjectName = TEXT("Héro"); // non-ASCII: UTF-8 on the wire
		Meta.NumChannels = 2;
		Meta.SampleRate = 44100;
		Meta.TimestampSec = 12.5;
		return Meta;
	}

	void CheckMeta(FAutomationTestBase& Test, const FString& Context, const O3DS::FAudioFrameMeta& Got, const O3DS::FAudioFrameMeta& Want)
	{
		Test.TestTrue(*FString::Printf(TEXT("%s: source guid"), *Context), Got.SourceGuid == Want.SourceGuid);
		Test.TestEqual(*FString::Printf(TEXT("%s: stream label"), *Context), Got.StreamLabel, Want.StreamLabel);
		Test.TestEqual(*FString::Printf(TEXT("%s: subject"), *Context), Got.SubjectName, Want.SubjectName);
		Test.TestEqual(*FString::Printf(TEXT("%s: channels"), *Context), Got.NumChannels, Want.NumChannels);
		Test.TestEqual(*FString::Printf(TEXT("%s: sample rate"), *Context), Got.SampleRate, Want.SampleRate);
		Test.TestEqual(*FString::Printf(TEXT("%s: timestamp"), *Context), Got.TimestampSec, Want.TimestampSec);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedUnifiedMessageRoundTripTest, "Open3DBroadcast.Shared.Parsers.UnifiedMessage.RoundTrip", O3DB_TEST_FLAGS)
bool FO3DSharedUnifiedMessageRoundTripTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Payload;
	for (int32 Index = 0; Index < 37; ++Index)
	{
		Payload.Add(static_cast<uint8>(Index * 7));
	}

	TArray<uint8> Message;
	if (!TestTrue(TEXT("Message created"), O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Audio, O3DS::EUnifiedCodec::Opus, Payload.GetData(), Payload.Num(), 1.5, Message)))
	{
		return false;
	}
	TestEqual(TEXT("20-byte header plus payload"), Message.Num(), 20 + Payload.Num());

	O3DS::FUnifiedHeader Header;
	const uint8* ParsedPayload = nullptr;
	int32 ParsedSize = 0;
	if (!TestTrue(TEXT("Message parses"), O3DS::ParseUnifiedMessage(Message.GetData(), Message.Num(), Header, ParsedPayload, ParsedSize)))
	{
		return false;
	}
	TestTrue(TEXT("Kind"), Header.GetKind() == O3DS::EUnifiedKind::Audio);
	TestTrue(TEXT("Codec"), Header.GetCodec() == O3DS::EUnifiedCodec::Opus);
	TestEqual(TEXT("Timestamp in microseconds"), Header.TimestampUs(), static_cast<uint64>(1500000));
	TestEqual(TEXT("Payload size"), ParsedSize, Payload.Num());
	TestTrue(TEXT("Payload points into the message after the header"), ParsedPayload == Message.GetData() + 20);
	TestTrue(TEXT("Payload bytes"), ParsedPayload != nullptr && FMemory::Memcmp(ParsedPayload, Payload.GetData(), Payload.Num()) == 0);

	// A message followed by trailing bytes still parses; the payload size comes from the header.
	TArray<uint8> WithTrailer = Message;
	WithTrailer.Add(0xAB);
	TestTrue(TEXT("Trailing bytes are ignored"), O3DS::ParseUnifiedMessage(WithTrailer.GetData(), WithTrailer.Num(), Header, ParsedPayload, ParsedSize) && ParsedSize == Payload.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedUnifiedMessageRejectsTest, "Open3DBroadcast.Shared.Parsers.UnifiedMessage.RejectsMalformed", O3DB_TEST_FLAGS)
bool FO3DSharedUnifiedMessageRejectsTest::RunTest(const FString& Parameters)
{
	const uint8 Payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	TArray<uint8> Message;
	if (!TestTrue(TEXT("Message created"), O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Mocap, O3DS::EUnifiedCodec::O3DS, Payload, 8, 0.0, Message)))
	{
		return false;
	}

	O3DS::FUnifiedHeader Header;
	const uint8* ParsedPayload = nullptr;
	int32 ParsedSize = 0;

	int32 AcceptedTruncations = 0;
	for (int32 Length = 0; Length < Message.Num(); ++Length)
	{
		AcceptedTruncations += O3DS::ParseUnifiedMessage(Message.GetData(), Length, Header, ParsedPayload, ParsedSize) ? 1 : 0;
	}
	TestEqual(TEXT("Every truncation is rejected"), AcceptedTruncations, 0);
	TestFalse(TEXT("Null data is rejected"), O3DS::ParseUnifiedMessage(nullptr, Message.Num(), Header, ParsedPayload, ParsedSize));

	TArray<uint8> BadMagic = Message;
	BadMagic[0] ^= 0xFF;
	TestFalse(TEXT("Bad magic is rejected"), O3DS::ParseUnifiedMessage(BadMagic.GetData(), BadMagic.Num(), Header, ParsedPayload, ParsedSize));

	// A header that claims more payload than the buffer holds.
	TArray<uint8> Oversize = Message;
	O3DS::WriteBE32(Oversize.GetData() + 16, 0x7FFFFFFFu);
	TestFalse(TEXT("A payload size past the end is rejected"), O3DS::ParseUnifiedMessage(Oversize.GetData(), Oversize.Num(), Header, ParsedPayload, ParsedSize));

	TArray<uint8> Unused;
	TestFalse(TEXT("An empty payload is not wrapped"), O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Mocap, O3DS::EUnifiedCodec::O3DS, Payload, 0, 0.0, Unused));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedPcm16FrameTest, "Open3DBroadcast.Shared.Parsers.Pcm16Frame.RoundTripAndTruncation", O3DB_TEST_FLAGS)
bool FO3DSharedPcm16FrameTest::RunTest(const FString& Parameters)
{
	using namespace O3DSharedParserTests;
	const O3DS::FAudioFrameMeta Meta = MakeMeta();
	const int16 Samples[6] = { 0, 1000, -1000, 32767, -32768, 42 };

	TArray<uint8> Wire;
	if (!TestTrue(TEXT("Serialize"), O3DAudio::SerializePcm16Frame(Meta, reinterpret_cast<const uint8*>(Samples), sizeof(Samples), Wire)))
	{
		return false;
	}

	O3DAudio::FPcm16Frame Frame;
	if (!TestTrue(TEXT("Deserialize"), O3DAudio::DeserializePcm16Frame(Wire.GetData(), Wire.Num(), Frame)))
	{
		return false;
	}
	CheckMeta(*this, TEXT("PCM16"), Frame.Meta, Meta);
	TestTrue(TEXT("PCM16 bytes"), Frame.PCM16.Num() == sizeof(Samples) && FMemory::Memcmp(Frame.PCM16.GetData(), Samples, sizeof(Samples)) == 0);

	int32 AcceptedTruncations = 0;
	for (int32 Length = 0; Length < Wire.Num(); ++Length)
	{
		O3DAudio::FPcm16Frame Ignored;
		AcceptedTruncations += O3DAudio::DeserializePcm16Frame(Wire.GetData(), Length, Ignored) ? 1 : 0;
	}
	TestEqual(TEXT("Every truncation is rejected"), AcceptedTruncations, 0);

	TArray<uint8> WrongVersion = Wire;
	WrongVersion[0] = 99;
	O3DAudio::FPcm16Frame Ignored;
	TestFalse(TEXT("Unknown version is rejected"), O3DAudio::DeserializePcm16Frame(WrongVersion.GetData(), WrongVersion.Num(), Ignored));

	TArray<uint8> Unused;
	TestFalse(TEXT("An odd PCM16 byte count is not serialized"), O3DAudio::SerializePcm16Frame(Meta, reinterpret_cast<const uint8*>(Samples), 3, Unused));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedEncodedFrameTest, "Open3DBroadcast.Shared.Parsers.EncodedAudioFrame.RoundTripAndCodecCheck", O3DB_TEST_FLAGS)
bool FO3DSharedEncodedFrameTest::RunTest(const FString& Parameters)
{
	using namespace O3DSharedParserTests;
	const O3DS::FAudioFrameMeta Meta = MakeMeta();
	const uint8 Encoded[11] = { 0xF8, 0xFF, 0xFE, 1, 2, 3, 4, 5, 6, 7, 8 }; // opaque; the codec is not run

	TArray<uint8> Wire;
	if (!TestTrue(TEXT("Serialize"), O3DAudio::SerializeEncodedAudioFrame(O3DS::EUnifiedCodec::Opus, Meta, Encoded, sizeof(Encoded), Wire)))
	{
		return false;
	}

	O3DAudio::FEncodedAudioFrame Frame;
	if (!TestTrue(TEXT("Deserialize as Opus"), O3DAudio::DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec::Opus, Wire.GetData(), Wire.Num(), Frame)))
	{
		return false;
	}
	TestTrue(TEXT("Codec kept"), Frame.Codec == O3DS::EUnifiedCodec::Opus);
	CheckMeta(*this, TEXT("Opus"), Frame.Meta, Meta);
	TestTrue(TEXT("Encoded bytes"), Frame.Payload.Num() == sizeof(Encoded) && FMemory::Memcmp(Frame.Payload.GetData(), Encoded, sizeof(Encoded)) == 0);

	O3DAudio::FEncodedAudioFrame Ignored;
	TestFalse(TEXT("A frame read with the wrong codec is rejected"), O3DAudio::DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec::O3DS, Wire.GetData(), Wire.Num(), Ignored));

	int32 AcceptedTruncations = 0;
	for (int32 Length = 0; Length < Wire.Num(); ++Length)
	{
		AcceptedTruncations += O3DAudio::DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec::Opus, Wire.GetData(), Length, Ignored) ? 1 : 0;
	}
	TestEqual(TEXT("Every truncation is rejected"), AcceptedTruncations, 0);

	// PCM16 through the generic entry points uses the PCM16 layout.
	const int16 Pcm[2] = { 7, -7 };
	TArray<uint8> PcmWire;
	TestTrue(TEXT("PCM16 via the encoded API"), O3DAudio::SerializeEncodedAudioFrame(O3DS::EUnifiedCodec::PCM16, Meta, reinterpret_cast<const uint8*>(Pcm), sizeof(Pcm), PcmWire));
	O3DAudio::FEncodedAudioFrame PcmFrame;
	TestTrue(TEXT("PCM16 back via the encoded API"), O3DAudio::DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec::PCM16, PcmWire.GetData(), PcmWire.Num(), PcmFrame) && PcmFrame.Payload.Num() == sizeof(Pcm));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
