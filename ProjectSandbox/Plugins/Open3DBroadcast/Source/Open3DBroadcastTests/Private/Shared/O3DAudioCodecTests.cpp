// Copyright (c) Open3DStream Contributors
//
// WP-S10: audio codec correctness.
// - SHR-1: a frame's codec label always matches its payload; Opus that is unavailable (compiled
//   out, or not possible at the stream's format) yields PCM16 labelled PCM16.
// - SHR-2: capture buffers of any size (512 and 1024 frames here) are accumulated into exact
//   Opus frames, and an Opus problem never silently disables Opus for the stream.
// - SHR-31: encoder settings are validated and the decoder accepts 120 ms packets.
// These run on every platform; the Opus-specific assertions switch on O3D_WITH_OPUS.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioFrameCodec.h"
#include "O3DAudioOpus.h"
#include "O3DAudioSerialization.h"
#include "O3DSinkAudioEncoder.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

#include <cmath>

namespace O3DAudioCodecTests
{
	constexpr int32 SampleRate = 48000;
	constexpr int32 OpusPacketFrames = SampleRate / 1000 * O3DAudio::FFrameEncoder::OpusFrameSizeMs; // 960

	FO3DTransportAudioConfig MakeConfig(const TCHAR* Codec, int32 InSampleRate, int32 InChannels)
	{
		FO3DTransportAudioConfig Config;
		Config.bEnableAudio = true;
		Config.Codec = Codec;
		Config.SampleRate = InSampleRate;
		Config.NumChannels = InChannels;
		Config.BitrateKbps = 64;
		return Config;
	}

	/** Interleaved test tone: two sines, well inside full scale. */
	TArray<float> MakeTone(int32 NumFrames, int32 NumChannels, int32 InSampleRate)
	{
		TArray<float> Samples;
		Samples.SetNumUninitialized(NumFrames * NumChannels);
		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			const double T = static_cast<double>(Frame) / static_cast<double>(InSampleRate);
			const float Value = static_cast<float>(0.5 * FMath::Sin(2.0 * UE_DOUBLE_PI * 440.0 * T) + 0.25 * FMath::Sin(2.0 * UE_DOUBLE_PI * 1250.0 * T));
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				Samples[Frame * NumChannels + Channel] = Value;
			}
		}
		return Samples;
	}

	/**
	 * Feeds Total frames of mono 48 kHz tone as BufferFrames-sized capture buffers through an
	 * encoder that requests Opus, and checks what comes out.
	 */
	void RunOpusFramingCase(FAutomationTestBase& Test, int32 BufferFrames)
	{
		const FString Context = FString::Printf(TEXT("%d-frame buffers"), BufferFrames);
		const int32 NumBuffers = 30;
		const int32 TotalFrames = BufferFrames * NumBuffers;
		const TArray<float> Tone = MakeTone(TotalFrames, 1, SampleRate);
		const double StartTimestamp = 100.0;

		O3DAudio::FFrameEncoder Encoder;
		Test.TestTrue(*FString::Printf(TEXT("%s: initialize"), *Context), Encoder.Initialize(MakeConfig(TEXT("Opus"), SampleRate, 1), TEXT("label"), TEXT("subject")));

		TArray<O3DAudio::FEncodedFrame> Frames;
		int32 CallsWithoutOutput = 0;
		for (int32 Buffer = 0; Buffer < NumBuffers; ++Buffer)
		{
			const int32 Before = Frames.Num();
			const double BufferTimestamp = StartTimestamp + static_cast<double>(Buffer * BufferFrames) / SampleRate;
			if (!Encoder.EncodeBuffer(FString(), FString(), Tone.GetData() + Buffer * BufferFrames, BufferFrames, 1, SampleRate, BufferTimestamp, Frames))
			{
				Test.AddError(FString::Printf(TEXT("%s: EncodeBuffer failed on buffer %d"), *Context, Buffer));
				return;
			}
			CallsWithoutOutput += (Frames.Num() == Before) ? 1 : 0;
		}

#if O3D_WITH_OPUS
		const int32 ExpectedPackets = TotalFrames / OpusPacketFrames;
		Test.TestEqual(*FString::Printf(TEXT("%s: one packet per whole Opus frame"), *Context), Frames.Num(), ExpectedPackets);
		Test.TestEqual(*FString::Printf(TEXT("%s: the remainder waits for the next packet"), *Context), Encoder.GetPendingFrames(), TotalFrames % OpusPacketFrames);
		if (BufferFrames < OpusPacketFrames)
		{
			Test.TestTrue(*FString::Printf(TEXT("%s: some calls only buffer samples"), *Context), CallsWithoutOutput > 0);
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: no encode failures"), *Context), Encoder.GetStats().OpusEncodeFailures, static_cast<uint64>(0));

		O3DAudio::FFrameDecoder Decoder;
		TArray<int16> Decoded;
		TArray<int16> Pcm;
		for (int32 Index = 0; Index < Frames.Num(); ++Index)
		{
			const O3DAudio::FEncodedFrame& Frame = Frames[Index];
			if (Frame.Codec != O3DS::EUnifiedCodec::Opus)
			{
				Test.AddError(FString::Printf(TEXT("%s: frame %d is not labelled Opus"), *Context, Index));
				return;
			}
			// Each packet carries the capture time of its first sample.
			const double ExpectedTimestamp = StartTimestamp + static_cast<double>(Index * OpusPacketFrames) / SampleRate;
			if (!FMath::IsNearlyEqual(Frame.Meta.TimestampSec, ExpectedTimestamp, 1e-9))
			{
				Test.AddError(FString::Printf(TEXT("%s: frame %d timestamp %.9f, expected %.9f"), *Context, Index, Frame.Meta.TimestampSec, ExpectedTimestamp));
			}
			if (!Decoder.Decode(Frame.Codec, Frame.Meta, Frame.Encoded.GetData(), Frame.Encoded.Num(), Pcm) || Pcm.Num() != OpusPacketFrames)
			{
				Test.AddError(FString::Printf(TEXT("%s: frame %d does not decode to one 20 ms Opus frame"), *Context, Index));
				return;
			}
			Decoded.Append(Pcm);
		}

		// The packets are one continuous stream: after the codec delay, the decoded audio
		// matches the input (SHR-32 method: compensate the lookahead, then measure SNR).
		FO3DAudioOpusEncoder Probe;
		FO3DAudioOpusEncoder::FSettings ProbeSettings;
		FString Error;
		Test.TestTrue(TEXT("Lookahead probe"), Probe.Initialize(ProbeSettings, Error));
		const int32 Lookahead = Probe.GetLookaheadSamples();
		double Signal = 0.0;
		double Noise = 0.0;
		for (int32 Index = OpusPacketFrames; Index + Lookahead < Decoded.Num(); ++Index)
		{
			const double Reference = Tone[Index];
			const double Difference = static_cast<double>(Decoded[Index + Lookahead]) / 32767.0 - Reference;
			Signal += Reference * Reference;
			Noise += Difference * Difference;
		}
		const double SnrDb = Noise > 0.0 ? 10.0 * std::log10(Signal / Noise) : 200.0;
		Test.TestTrue(*FString::Printf(TEXT("%s: decoded stream SNR %.1f dB > 20 dB"), *Context, SnrDb), SnrDb > 20.0);
#else
		static_assert(OpusPacketFrames == 960, "20 ms at 48 kHz");
		// Opus compiled out: every buffer goes out at once as PCM16, labelled PCM16.
		Test.TestEqual(*FString::Printf(TEXT("%s: one PCM16 frame per buffer"), *Context), Frames.Num(), NumBuffers);
		Test.TestEqual(*FString::Printf(TEXT("%s: nothing is held back"), *Context), CallsWithoutOutput, 0);
		for (const O3DAudio::FEncodedFrame& Frame : Frames)
		{
			if (Frame.Codec != O3DS::EUnifiedCodec::PCM16 || Frame.Encoded.Num() != BufferFrames * 2)
			{
				Test.AddError(FString::Printf(TEXT("%s: frame is not PCM16 of the buffer's size"), *Context));
				return;
			}
		}
#endif
	}

	/** Every frame must be PCM16, labelled PCM16, and carry exactly the input samples. */
	void CheckPcm16Frames(FAutomationTestBase& Test, const FString& Context, const TArray<O3DAudio::FEncodedFrame>& Frames, const TArray<float>& Input, int32 NumChannels, int32 InSampleRate)
	{
		TArray<int16> Expected;
		Expected.SetNumUninitialized(Input.Num());
		O3DAudio::ConvertFloatToPcm16(Input.GetData(), Input.Num(), Expected.GetData());

		TArray<int16> Got;
		O3DAudio::FFrameDecoder Decoder;
		TArray<int16> Pcm;
		for (const O3DAudio::FEncodedFrame& Frame : Frames)
		{
			if (Frame.Codec != O3DS::EUnifiedCodec::PCM16)
			{
				Test.AddError(FString::Printf(TEXT("%s: a frame is labelled %d, not PCM16"), *Context, static_cast<int32>(Frame.Codec)));
				return;
			}
			Test.TestEqual(*FString::Printf(TEXT("%s: channels in meta"), *Context), Frame.Meta.NumChannels, NumChannels);
			Test.TestEqual(*FString::Printf(TEXT("%s: sample rate in meta"), *Context), Frame.Meta.SampleRate, InSampleRate);
			if (!Decoder.Decode(Frame.Codec, Frame.Meta, Frame.Encoded.GetData(), Frame.Encoded.Num(), Pcm))
			{
				Test.AddError(FString::Printf(TEXT("%s: PCM16 frame does not decode"), *Context));
				return;
			}
			Got.Append(Pcm);
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: payload is the input as PCM16"), *Context), Got == Expected);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioEncoderOpusFraming512Test, "Open3DBroadcast.Shared.Audio.Encoder.OpusFraming512", O3DB_TEST_FLAGS)
bool FO3DAudioEncoderOpusFraming512Test::RunTest(const FString& Parameters)
{
	O3DAudioCodecTests::RunOpusFramingCase(*this, 512);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioEncoderOpusFraming1024Test, "Open3DBroadcast.Shared.Audio.Encoder.OpusFraming1024", O3DB_TEST_FLAGS)
bool FO3DAudioEncoderOpusFraming1024Test::RunTest(const FString& Parameters)
{
	O3DAudioCodecTests::RunOpusFramingCase(*this, 1024);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioEncoderOpusUnavailableTest, "Open3DBroadcast.Shared.Audio.Encoder.OpusUnavailableSendsLabelledPcm16", O3DB_TEST_FLAGS)
bool FO3DAudioEncoderOpusUnavailableTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioCodecTests;

	// Opus cannot run at 44.1 kHz or with 3 channels, whether or not it is compiled in. Before
	// SHR-1 these frames were labelled Opus while carrying PCM16.
	struct FCase
	{
		const TCHAR* Name;
		int32 Rate;
		int32 Channels;
	};
	const FCase Cases[] = {
		{ TEXT("44.1 kHz"), 44100, 1 },
		{ TEXT("3 channels"), 48000, 3 },
#if !O3D_WITH_OPUS
		// Compiled out: even a format Opus supports is sent as PCM16.
		{ TEXT("Opus compiled out"), 48000, 1 },
#endif
	};

	for (const FCase& Case : Cases)
	{
		O3DAudio::FFrameEncoder Encoder;
		Encoder.Initialize(MakeConfig(TEXT("Opus"), Case.Rate, Case.Channels), TEXT("label"), TEXT("subject"));

		const TArray<float> Input = MakeTone(512 * 3, Case.Channels, Case.Rate);
		TArray<O3DAudio::FEncodedFrame> Frames;
		for (int32 Buffer = 0; Buffer < 3; ++Buffer)
		{
			TestTrue(*FString::Printf(TEXT("%s: buffer accepted"), Case.Name),
				Encoder.EncodeBuffer(FString(), FString(), Input.GetData() + Buffer * 512 * Case.Channels, 512, Case.Channels, Case.Rate, Buffer * 0.01, Frames));
		}
		TestEqual(*FString::Printf(TEXT("%s: one PCM16 frame per buffer, nothing held back"), Case.Name), Frames.Num(), 3);
		TestEqual(*FString::Printf(TEXT("%s: no Opus packets"), Case.Name), Encoder.GetStats().OpusPackets, static_cast<uint64>(0));
		CheckPcm16Frames(*this, Case.Name, Frames, Input, Case.Channels, Case.Rate);

		// The same frames survive the wire with the right codec in both headers.
		TArray<uint8> Message;
		O3DS::FUnifiedHeader Header;
		const uint8* Payload = nullptr;
		int32 PayloadSize = 0;
		O3DAudio::FEncodedAudioFrame Parsed;
		const bool bWire = Frames.Num() > 0
			&& O3DAudio::CreateUnifiedAudioMessage(Frames[0], Frames[0].Meta.TimestampSec, Message)
			&& O3DS::ParseUnifiedMessage(Message.GetData(), Message.Num(), Header, Payload, PayloadSize)
			&& Header.GetCodec() == O3DS::EUnifiedCodec::PCM16
			&& O3DAudio::DeserializeEncodedAudioFrame(Header.GetCodec(), Payload, PayloadSize, Parsed)
			&& Parsed.Payload == Frames[0].Encoded;
		TestTrue(*FString::Printf(TEXT("%s: envelope says PCM16 and round-trips"), Case.Name), bWire);
	}

	// PCM16 requested: always PCM16, one frame per buffer, on every platform.
	O3DAudio::FFrameEncoder Pcm16Encoder;
	Pcm16Encoder.Initialize(MakeConfig(TEXT("PCM16"), 48000, 2), TEXT("label"), TEXT("subject"));
	const TArray<float> Stereo = MakeTone(1024, 2, 48000);
	TArray<O3DAudio::FEncodedFrame> Pcm16Frames;
	TestTrue(TEXT("PCM16 buffer accepted"), Pcm16Encoder.EncodeBuffer(FString(), FString(), Stereo.GetData(), 1024, 2, 48000, 1.0, Pcm16Frames));
	TestEqual(TEXT("PCM16: one frame"), Pcm16Frames.Num(), 1);
	CheckPcm16Frames(*this, TEXT("PCM16 requested"), Pcm16Frames, Stereo, 2, 48000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioSinkEncoderUnifiedTest, "Open3DBroadcast.Shared.Audio.Encoder.SinkEncoderUnifiedMessages", O3DB_TEST_FLAGS)
bool FO3DAudioSinkEncoderUnifiedTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioCodecTests;

	FO3DSinkAudioEncoder::FSettings Settings;
	Settings.Config = MakeConfig(TEXT("Opus"), SampleRate, 1);
	Settings.DefaultSubject = TEXT("hero");
	Settings.SourceGuid = FGuid::NewGuid();
	FO3DSinkAudioEncoder Encoder(Settings);

	const TArray<float> Tone = MakeTone(1024 * 4, 1, SampleRate);
	int32 Messages = 0;
	for (int32 Buffer = 0; Buffer < 4; ++Buffer)
	{
		TArray<TArray<uint8>> Out;
		if (!TestTrue(TEXT("EncodeUnified"), Encoder.EncodeUnified(TEXT("voice"), FString(), Tone.GetData() + Buffer * 1024, 1024, 1, SampleRate, Buffer * (1024.0 / SampleRate), Out)))
		{
			return false;
		}
		for (const TArray<uint8>& Message : Out)
		{
			++Messages;
			O3DS::FUnifiedHeader Header;
			const uint8* Payload = nullptr;
			int32 PayloadSize = 0;
			O3DAudio::FEncodedAudioFrame Parsed;
			O3DAudio::EAudioParseError ParseError = O3DAudio::EAudioParseError::None;
			const bool bParsed = O3DS::ParseUnifiedMessage(Message.GetData(), Message.Num(), Header, Payload, PayloadSize)
				&& PayloadSize == Message.Num() - O3DS::UnifiedWireHeaderSize
				&& O3DAudio::DeserializeEncodedAudioFrame(Header.GetCodec(), Payload, PayloadSize, Parsed, &ParseError);
			if (!bParsed)
			{
				AddError(FString::Printf(TEXT("Message %d does not parse (error %d)"), Messages, static_cast<int32>(ParseError)));
				return false;
			}
			TestTrue(TEXT("Envelope and payload agree on the codec"), Parsed.Codec == Header.GetCodec());
			TestTrue(TEXT("SourceGuid stamped"), Parsed.Meta.SourceGuid == Settings.SourceGuid);
			TestEqual(TEXT("Label"), Parsed.Meta.StreamLabel, FString(TEXT("voice")));
			TestEqual(TEXT("Subject"), Parsed.Meta.SubjectName, FString(TEXT("hero")));
			TestEqual(TEXT("Envelope timestamp is the frame's"), Header.TimestampUs(), static_cast<uint64>(Parsed.Meta.TimestampSec * 1000000.0));
#if O3D_WITH_OPUS
			TestTrue(TEXT("Opus requested and available: Opus packets"), Header.GetCodec() == O3DS::EUnifiedCodec::Opus);
#else
			TestTrue(TEXT("Opus compiled out: PCM16"), Header.GetCodec() == O3DS::EUnifiedCodec::PCM16);
#endif
		}
	}
#if O3D_WITH_OPUS
	TestEqual(TEXT("4096 frames make four 960-frame packets"), Messages, 4);
#else
	TestEqual(TEXT("One PCM16 message per buffer"), Messages, 4);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioOpusSettingsTest, "Open3DBroadcast.Shared.Audio.Opus.SettingsValidation", O3DB_TEST_FLAGS)
bool FO3DAudioOpusSettingsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("48 kHz is an Opus rate"), FO3DAudioOpusEncoder::IsSupportedSampleRate(48000));
	TestFalse(TEXT("44.1 kHz is not an Opus rate"), FO3DAudioOpusEncoder::IsSupportedSampleRate(44100));
	TestTrue(TEXT("20 ms is an Opus frame"), FO3DAudioOpusEncoder::IsSupportedFrameSizeMs(20));
	TestFalse(TEXT("15 ms is not an Opus frame"), FO3DAudioOpusEncoder::IsSupportedFrameSizeMs(15));

	FString Error;
	FO3DAudioOpusEncoder Encoder;
	FO3DAudioOpusEncoder::FSettings Settings;
	Settings.FrameSizeMs = 15;
	TestFalse(TEXT("15 ms frames are rejected"), Encoder.Initialize(Settings, Error));
	Settings.FrameSizeMs = 20;
	Settings.SampleRate = 44100;
	TestFalse(TEXT("44.1 kHz is rejected"), Encoder.Initialize(Settings, Error));

#if O3D_WITH_OPUS
	Settings.SampleRate = 48000;
	if (!TestTrue(TEXT("48 kHz, 20 ms initializes"), Encoder.Initialize(Settings, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Frame size in samples"), Encoder.GetFrameSizeSamples(), 960);
	TestTrue(TEXT("Lookahead is reported"), Encoder.GetLookaheadSamples() > 0);

	const TArray<float> Silence = [] { TArray<float> A; A.SetNumZeroed(960); return A; }();
	TArray<uint8> Packet;
	int32 FramesEncoded = 0;
	TestFalse(TEXT("A partial Opus frame is rejected, not passed to libOpus"), Encoder.Encode(Silence.GetData(), 512, Packet, FramesEncoded));
	TestTrue(TEXT("A whole Opus frame encodes"), Encoder.Encode(Silence.GetData(), 960, Packet, FramesEncoded) && Packet.Num() > 0 && Packet.Num() <= FO3DAudioOpusEncoder::MaxPacketBytes);

	FO3DAudioOpusDecoder Decoder;
	FO3DAudioOpusDecoder::FSettings DecoderSettings;
	TestTrue(TEXT("Decoder initializes"), Decoder.Initialize(DecoderSettings, Error));
	TestEqual(TEXT("Decoder accepts 120 ms packets by default"), Decoder.GetMaxFrameSizeSamples(), 5760);
	TArray<int16> Pcm;
	int32 FramesDecoded = 0;
	TestTrue(TEXT("Packet decodes"), Decoder.Decode(Packet.GetData(), Packet.Num(), Pcm, FramesDecoded) && FramesDecoded == 960);
	TestTrue(TEXT("Loss concealment produces the requested frames"), Decoder.DecodeLost(960, Pcm, FramesDecoded) && FramesDecoded == 960 && Pcm.Num() == 960);
#else
	Settings.SampleRate = 48000;
	TestFalse(TEXT("Without Opus the encoder never initializes"), Encoder.Initialize(Settings, Error));
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
