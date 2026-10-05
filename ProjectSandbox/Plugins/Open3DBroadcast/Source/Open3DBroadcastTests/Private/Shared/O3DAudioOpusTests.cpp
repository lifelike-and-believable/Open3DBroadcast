// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DTestHarness.h"
#include "O3DAudioOpus.h"

#include <cmath>

#if O3D_WITH_OPUS

// SHR-32: Opus delays its output by the encoder lookahead, so decoded sample i + Lookahead is
// compared with input sample i. The first packet (encoder warm-up) is not scored. PCM16 codec
// paths, which run on every platform, are covered in O3DAudioCodecTests.cpp.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioOpusRoundTripTest, "Open3DBroadcast.Shared.Audio.Opus.RoundTrip", O3DB_TEST_FLAGS)
bool FO3DAudioOpusRoundTripTest::RunTest(const FString& Parameters)
{
	FO3DAudioOpusEncoder::FSettings EncoderSettings;
	EncoderSettings.SampleRate = 48000;
	EncoderSettings.NumChannels = 1;
	EncoderSettings.FrameSizeMs = 20;
	EncoderSettings.BitrateKbps = 64;

	FO3DAudioOpusEncoder Encoder;
	FString Error;
	if (!TestTrue(TEXT("Encoder initialization should succeed"), Encoder.Initialize(EncoderSettings, Error)))
	{
		AddError(FString::Printf(TEXT("Encoder Initialize failed: %s"), *Error));
		return false;
	}

	const int32 FrameSamples = Encoder.GetFrameSizeSamples();
	const int32 Lookahead = Encoder.GetLookaheadSamples();
	TestEqual(TEXT("20 ms at 48 kHz"), FrameSamples, 960);
	TestTrue(TEXT("Lookahead is positive and shorter than a frame"), Lookahead > 0 && Lookahead < FrameSamples);

	const int32 NumPackets = 25;
	const int32 TotalInputSamples = FrameSamples * NumPackets;
	TArray<float> Input;
	Input.SetNumUninitialized(TotalInputSamples);
	for (int32 Index = 0; Index < TotalInputSamples; ++Index)
	{
		const double T = static_cast<double>(Index) / EncoderSettings.SampleRate;
		Input[Index] = static_cast<float>(0.5 * FMath::Sin(2.0 * UE_DOUBLE_PI * 440.0 * T) + 0.25 * FMath::Sin(2.0 * UE_DOUBLE_PI * 1250.0 * T));
	}

	FO3DAudioOpusDecoder::FSettings DecoderSettings;
	DecoderSettings.SampleRate = EncoderSettings.SampleRate;
	DecoderSettings.NumChannels = EncoderSettings.NumChannels;

	FO3DAudioOpusDecoder Decoder;
	if (!TestTrue(TEXT("Decoder initialization should succeed"), Decoder.Initialize(DecoderSettings, Error)))
	{
		AddError(FString::Printf(TEXT("Decoder Initialize failed: %s"), *Error));
		return false;
	}

	TArray<int16> Decoded;
	TArray<uint8> Packet;
	TArray<int16> PacketPcm;
	for (int32 PacketIndex = 0; PacketIndex < NumPackets; ++PacketIndex)
	{
		int32 FramesEncoded = 0;
		if (!Encoder.Encode(&Input[PacketIndex * FrameSamples], FrameSamples, Packet, FramesEncoded) || FramesEncoded != FrameSamples)
		{
			AddError(FString::Printf(TEXT("Packet %d: encode failed or encoded %d frames"), PacketIndex, FramesEncoded));
			return false;
		}

		int32 FramesDecoded = 0;
		if (!Decoder.Decode(Packet.GetData(), Packet.Num(), PacketPcm, FramesDecoded))
		{
			AddError(FString::Printf(TEXT("Packet %d: decode failed (%d bytes)"), PacketIndex, Packet.Num()));
			return false;
		}
		if (FramesDecoded != FrameSamples)
		{
			AddError(FString::Printf(TEXT("Packet %d: decoded %d frames, expected %d"), PacketIndex, FramesDecoded, FrameSamples));
			return false;
		}
		Decoded.Append(PacketPcm);
	}

	if (!TestEqual(TEXT("Decoded sample count matches the input"), Decoded.Num(), TotalInputSamples))
	{
		return false;
	}

	double Signal = 0.0;
	double Noise = 0.0;
	double AbsoluteError = 0.0;
	int32 Compared = 0;
	for (int32 Index = FrameSamples; Index + Lookahead < TotalInputSamples; ++Index)
	{
		const double Original = Input[Index];
		const double Difference = static_cast<double>(Decoded[Index + Lookahead]) / 32767.0 - Original;
		Signal += Original * Original;
		Noise += Difference * Difference;
		AbsoluteError += FMath::Abs(Difference);
		++Compared;
	}

	const double SnrDb = Noise > 0.0 ? 10.0 * std::log10(Signal / Noise) : 200.0;
	const double AverageError = AbsoluteError / FMath::Max(Compared, 1);
	AddInfo(FString::Printf(TEXT("Lookahead %d samples, SNR %.1f dB, average absolute error %.4f"), Lookahead, SnrDb, AverageError));
	TestTrue(FString::Printf(TEXT("SNR %.1f dB > 20 dB"), SnrDb), SnrDb > 20.0);
	TestTrue(FString::Printf(TEXT("Average absolute error %.4f < 0.02"), AverageError), AverageError < 0.02);
	return true;
}

#endif // O3D_WITH_OPUS

#endif // WITH_DEV_AUTOMATION_TESTS
