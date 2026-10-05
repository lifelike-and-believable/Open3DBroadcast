// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
//
// SHR-6 (WP-T2): direct tests for the Shared parsers that read untrusted network bytes: the
// unified message envelope (O3DUnifiedMessage.h) and the audio frame format
// (O3DAudioSerialization.h). Every truncation of a valid buffer must be rejected, and a round
// trip must keep every field.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DAudioSerialization.h"
#include "O3DUnifiedMessage.h"
#include "Math/RandomStream.h"

#include <limits>

namespace O3DSharedParserTests
{
	O3DS::FAudioFrameMeta MakeMeta()
	{
		O3DS::FAudioFrameMeta Meta;
		Meta.SourceGuid = FGuid(1, 2, 3, 4);
		Meta.StreamLabel = TEXT("o3ds:mix/hero");
		Meta.SubjectName = TEXT("Héro"); // non-ASCII: UTF-8 on the wire
		Meta.NumChannels = 2;
		Meta.SampleRate = 48000; // valid for Opus too (SHR-8 checks the rate per codec)
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
	TestEqual(TEXT("Envelope v2 header (24 bytes) plus payload"), Message.Num(), O3DS::UnifiedWireHeaderSize + Payload.Num());

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
	TestTrue(TEXT("Payload points into the message after the header"), ParsedPayload == Message.GetData() + O3DS::UnifiedWireHeaderSize);
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
	O3DS::WriteBE32(Oversize.GetData() + 16, 0x7FFFFFFFu); // read little-endian: 0xFFFFFF7F
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

namespace O3DSharedParserTests
{
	void PatchU16(TArray<uint8>& Wire, int32 Offset, uint16 Value)
	{
		Wire[Offset] = static_cast<uint8>(Value & 0xFF);
		Wire[Offset + 1] = static_cast<uint8>(Value >> 8);
	}

	void PatchU32(TArray<uint8>& Wire, int32 Offset, uint32 Value)
	{
		for (int32 Byte = 0; Byte < 4; ++Byte)
		{
			Wire[Offset + Byte] = static_cast<uint8>((Value >> (8 * Byte)) & 0xFF);
		}
	}

	void PatchDouble(TArray<uint8>& Wire, int32 Offset, double Value)
	{
		FMemory::Memcpy(Wire.GetData() + Offset, &Value, sizeof(double)); // the wire is little-endian, like the hosts under test
	}

	// Field offsets. PCM16 layout: version, flags, channels(2), rate(4), timestamp(8), guid(16),
	// label size(2), subject size(2), data bytes(4). The encoded layout adds codec and reserved
	// bytes after the flags, so its fields sit two bytes later.
	constexpr int32 Pcm16ChannelsOffset = 2;
	constexpr int32 Pcm16RateOffset = 4;
	constexpr int32 Pcm16TimestampOffset = 8;
	constexpr int32 Pcm16LabelSizeOffset = 32;
	constexpr int32 Pcm16DataBytesOffset = 36;
	constexpr int32 EncodedFieldShift = 2;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedAudioMetaValidationTest, "Open3DBroadcast.Shared.Parsers.AudioMeta.RejectsOutOfRange", O3DB_TEST_FLAGS)
bool FO3DSharedAudioMetaValidationTest::RunTest(const FString& Parameters)
{
	using namespace O3DSharedParserTests;
	using O3DAudio::EAudioParseError;

	// SHR-8: channel count, sample rate, timestamp and name lengths from the wire are
	// range-checked before use, and the parser says why it rejected a buffer.
	const O3DS::FAudioFrameMeta Meta = MakeMeta();
	const int16 Samples[4] = { 1, -1, 2, -2 };
	const uint8 OpusBytes[6] = { 0xF8, 1, 2, 3, 4, 5 };

	TArray<uint8> PcmWire;
	TArray<uint8> OpusWire;
	if (!TestTrue(TEXT("Serialize PCM16"), O3DAudio::SerializePcm16Frame(Meta, reinterpret_cast<const uint8*>(Samples), sizeof(Samples), PcmWire))
		|| !TestTrue(TEXT("Serialize Opus"), O3DAudio::SerializeEncodedAudioFrame(O3DS::EUnifiedCodec::Opus, Meta, OpusBytes, sizeof(OpusBytes), OpusWire)))
	{
		return false;
	}

	auto ParsePcm = [](const TArray<uint8>& Wire)
	{
		O3DAudio::FPcm16Frame Frame;
		EAudioParseError Error = EAudioParseError::None;
		O3DAudio::DeserializePcm16Frame(Wire.GetData(), Wire.Num(), Frame, &Error);
		return Error;
	};
	auto ParseOpus = [](const TArray<uint8>& Wire)
	{
		O3DAudio::FEncodedAudioFrame Frame;
		EAudioParseError Error = EAudioParseError::None;
		O3DAudio::DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec::Opus, Wire.GetData(), Wire.Num(), Frame, &Error);
		return Error;
	};
	auto Check = [this](const TCHAR* What, EAudioParseError Got, EAudioParseError Want)
	{
		TestEqual(What, static_cast<int32>(Got), static_cast<int32>(Want));
	};

	Check(TEXT("Valid PCM16 parses"), ParsePcm(PcmWire), EAudioParseError::None);
	Check(TEXT("Valid Opus parses"), ParseOpus(OpusWire), EAudioParseError::None);

	{
		TArray<uint8> Wire = PcmWire;
		PatchU16(Wire, Pcm16ChannelsOffset, 0);
		Check(TEXT("PCM16 with 0 channels"), ParsePcm(Wire), EAudioParseError::BadChannelCount);
		PatchU16(Wire, Pcm16ChannelsOffset, 9);
		Check(TEXT("PCM16 with 9 channels"), ParsePcm(Wire), EAudioParseError::BadChannelCount);
		PatchU16(Wire, Pcm16ChannelsOffset, 8);
		Check(TEXT("PCM16 with 8 channels is accepted"), ParsePcm(Wire), EAudioParseError::None);
	}
	{
		TArray<uint8> Wire = OpusWire;
		PatchU16(Wire, Pcm16ChannelsOffset + EncodedFieldShift, 3);
		Check(TEXT("Opus with 3 channels"), ParseOpus(Wire), EAudioParseError::BadChannelCount);
	}
	{
		TArray<uint8> Wire = PcmWire;
		PatchU32(Wire, Pcm16RateOffset, 12345);
		Check(TEXT("PCM16 at 12345 Hz"), ParsePcm(Wire), EAudioParseError::BadSampleRate);
		PatchU32(Wire, Pcm16RateOffset, 0xFFFFFFFFu);
		Check(TEXT("PCM16 at 2^32-1 Hz (negative as int32)"), ParsePcm(Wire), EAudioParseError::BadSampleRate);
		PatchU32(Wire, Pcm16RateOffset, 44100);
		Check(TEXT("PCM16 at 44.1 kHz is accepted"), ParsePcm(Wire), EAudioParseError::None);
	}
	{
		TArray<uint8> Wire = OpusWire;
		PatchU32(Wire, Pcm16RateOffset + EncodedFieldShift, 44100);
		Check(TEXT("Opus at 44.1 kHz"), ParseOpus(Wire), EAudioParseError::BadSampleRate);
	}
	{
		TArray<uint8> Wire = PcmWire;
		PatchDouble(Wire, Pcm16TimestampOffset, std::numeric_limits<double>::quiet_NaN());
		Check(TEXT("NaN timestamp"), ParsePcm(Wire), EAudioParseError::BadTimestamp);
		PatchDouble(Wire, Pcm16TimestampOffset, std::numeric_limits<double>::infinity());
		Check(TEXT("Infinite timestamp"), ParsePcm(Wire), EAudioParseError::BadTimestamp);
	}
	{
		TArray<uint8> Wire = OpusWire;
		PatchDouble(Wire, Pcm16TimestampOffset + EncodedFieldShift, -std::numeric_limits<double>::infinity());
		Check(TEXT("Opus with an infinite timestamp"), ParseOpus(Wire), EAudioParseError::BadTimestamp);
	}

	// Names: the sender refuses what the receiver would reject.
	O3DS::FAudioFrameMeta LongMeta = Meta;
	LongMeta.StreamLabel = FString::ChrN(O3DAudio::MaxNameBytes + 1, TEXT('a'));
	TArray<uint8> Unused;
	TestFalse(TEXT("A 257-byte label is not serialized"), O3DAudio::SerializePcm16Frame(LongMeta, reinterpret_cast<const uint8*>(Samples), sizeof(Samples), Unused));
	LongMeta.StreamLabel = FString::ChrN(O3DAudio::MaxNameBytes, TEXT('a'));
	TArray<uint8> LongWire;
	TArray<int16> ManySamples;
	ManySamples.SetNumZeroed(50);
	if (TestTrue(TEXT("A 256-byte label is serialized"), O3DAudio::SerializePcm16Frame(LongMeta, reinterpret_cast<const uint8*>(ManySamples.GetData()), ManySamples.Num() * 2, LongWire)))
	{
		Check(TEXT("256-byte label parses"), ParsePcm(LongWire), EAudioParseError::None);
		// Claim a 300-byte label and a 2-byte payload: still inside the buffer, but too long.
		PatchU16(LongWire, Pcm16LabelSizeOffset, 300);
		PatchU32(LongWire, Pcm16DataBytesOffset, 2);
		Check(TEXT("A 300-byte label from the wire"), ParsePcm(LongWire), EAudioParseError::NameTooLong);
	}

	Check(TEXT("Truncated buffer"), ParsePcm(TArray<uint8>(PcmWire.GetData(), 10)), EAudioParseError::Truncated);
	{
		TArray<uint8> Wire = OpusWire;
		Wire[2] = static_cast<uint8>(O3DS::EUnifiedCodec::PCM16);
		Check(TEXT("Codec byte disagrees with the envelope"), ParseOpus(Wire), EAudioParseError::CodecMismatch);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSharedAudioParserFuzzTest, "Open3DBroadcast.Shared.Parsers.AudioMeta.RandomBytes", O3DB_TEST_FLAGS)
bool FO3DSharedAudioParserFuzzTest::RunTest(const FString& Parameters)
{
	using namespace O3DSharedParserTests;

	// Random buffers and random corruptions of valid ones never crash the parsers, and whatever
	// they accept has in-range metadata (SHR-8). Fixed seed: deterministic.
	FRandomStream Random(0x5A10);
	const O3DS::FAudioFrameMeta Meta = MakeMeta();
	const int16 Samples[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	TArray<uint8> Valid;
	TestTrue(TEXT("Serialize seed frame"), O3DAudio::SerializeEncodedAudioFrame(O3DS::EUnifiedCodec::PCM16, Meta, reinterpret_cast<const uint8*>(Samples), sizeof(Samples), Valid));

	int32 Accepted = 0;
	int32 AcceptedOutOfRange = 0;
	for (int32 Iteration = 0; Iteration < 4000; ++Iteration)
	{
		TArray<uint8> Wire;
		if (Iteration % 2 == 0)
		{
			Wire.SetNumUninitialized(Random.RandRange(0, 160));
			for (uint8& Byte : Wire)
			{
				Byte = static_cast<uint8>(Random.RandRange(0, 255));
			}
			if (Wire.Num() > 0)
			{
				Wire[0] = static_cast<uint8>(Random.RandRange(1, 2)); // reach past the version check
			}
		}
		else
		{
			Wire = Valid;
			const int32 Flips = Random.RandRange(1, 4);
			for (int32 Flip = 0; Flip < Flips; ++Flip)
			{
				const int32 Index = Random.RandRange(0, Wire.Num() - 1);
				Wire[Index] = static_cast<uint8>(Wire[Index] ^ (1 << Random.RandRange(0, 7)));
			}
		}

		for (const O3DS::EUnifiedCodec Codec : { O3DS::EUnifiedCodec::PCM16, O3DS::EUnifiedCodec::Opus })
		{
			O3DAudio::FEncodedAudioFrame Frame;
			if (O3DAudio::DeserializeEncodedAudioFrame(Codec, Wire.GetData(), Wire.Num(), Frame))
			{
				++Accepted;
				AcceptedOutOfRange += (O3DAudio::ValidateAudioMeta(Codec, Frame.Meta) != O3DAudio::EAudioParseError::None) ? 1 : 0;
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d of 8000 random parses accepted"), Accepted));
	TestEqual(TEXT("Nothing accepted has out-of-range metadata"), AcceptedOutOfRange, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
