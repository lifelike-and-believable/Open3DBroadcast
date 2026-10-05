// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
//
// SND-21 (WP-S10): the sender's resampler keeps its state across capture buffers (no drift, no
// seam at buffer boundaries) and low-pass filters before downsampling.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Containers/ArrayView.h"
#include "O3DAudioResampler.h"

namespace O3DAudioResamplerTests
{
	TArray<float> MakeSine(int32 NumFrames, int32 NumChannels, int32 SampleRate, double FrequencyHz)
	{
		TArray<float> Samples;
		Samples.SetNumUninitialized(NumFrames * NumChannels);
		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				const double Phase = 2.0 * UE_DOUBLE_PI * FrequencyHz * Frame / SampleRate + Channel;
				Samples[Frame * NumChannels + Channel] = static_cast<float>(FMath::Sin(Phase));
			}
		}
		return Samples;
	}

	/** Resample Input in chunks whose sizes cycle through ChunkSizes; concatenate the output. */
	TArray<float> ResampleInChunks(FO3DAudioResampler& Resampler, const TArray<float>& Input, int32 NumChannels, TConstArrayView<int32> ChunkSizes)
	{
		TArray<float> Output;
		TArray<float> Chunk;
		const int32 TotalFrames = Input.Num() / NumChannels;
		int32 Consumed = 0;
		int32 ChunkIndex = 0;
		while (Consumed < TotalFrames)
		{
			const int32 Frames = FMath::Min(ChunkSizes[ChunkIndex++ % ChunkSizes.Num()], TotalFrames - Consumed);
			double Offset = 0.0;
			Resampler.Process(Input.GetData() + Consumed * NumChannels, Frames, Chunk, Offset);
			Output.Append(Chunk);
			Consumed += Frames;
		}
		return Output;
	}

	double Rms(const TArray<float>& Samples, int32 SkipSamples)
	{
		double Sum = 0.0;
		int32 Count = 0;
		for (int32 Index = SkipSamples; Index < Samples.Num(); ++Index)
		{
			Sum += static_cast<double>(Samples[Index]) * Samples[Index];
			++Count;
		}
		return Count > 0 ? FMath::Sqrt(Sum / Count) : 0.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioResamplerContinuityTest, "Open3DBroadcast.Shared.Audio.Resampler.ChunkInvariantNoDrift", O3DB_TEST_FLAGS)
bool FO3DAudioResamplerContinuityTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioResamplerTests;

	// 10 s of 44.1 kHz stereo to 48 kHz. The old per-buffer resampler rounded each buffer's
	// length and restarted interpolation at every buffer.
	const int32 InRate = 44100;
	const int32 OutRate = 48000;
	const int32 Channels = 2;
	const TArray<float> Input = MakeSine(InRate * 10, Channels, InRate, 440.0);

	FO3DAudioResampler Whole;
	TestTrue(TEXT("Configure"), Whole.Configure(InRate, OutRate, Channels));
	const int32 WholeSizes[] = { InRate * 10 };
	const TArray<float> WholeOut = ResampleInChunks(Whole, Input, Channels, WholeSizes);

	FO3DAudioResampler Chunked;
	Chunked.Configure(InRate, OutRate, Channels);
	const int32 CaptureSizes[] = { 441 };
	const TArray<float> ChunkedOut = ResampleInChunks(Chunked, Input, Channels, CaptureSizes);

	FO3DAudioResampler Irregular;
	Irregular.Configure(InRate, OutRate, Channels);
	const int32 IrregularSizes[] = { 1, 7, 512, 1024, 333 };
	const TArray<float> IrregularOut = ResampleInChunks(Irregular, Input, Channels, IrregularSizes);

	const int32 OutFrames = WholeOut.Num() / Channels;
	TestTrue(FString::Printf(TEXT("No drift: %d output frames for 10 s"), OutFrames), FMath::Abs(OutFrames - OutRate * 10) <= 1);
	TestTrue(TEXT("441-frame buffers give the same samples as one buffer"), ChunkedOut == WholeOut);
	TestTrue(TEXT("Irregular buffers give the same samples as one buffer"), IrregularOut == WholeOut);

	// Downsampling path (with the anti-aliasing filter) is chunk-invariant too.
	const TArray<float> Mono48k = MakeSine(48000 * 2, 1, 48000, 1000.0);
	FO3DAudioResampler DownWhole;
	DownWhole.Configure(48000, 16000, 1);
	const int32 DownWholeSizes[] = { 48000 * 2 };
	const TArray<float> DownWholeOut = ResampleInChunks(DownWhole, Mono48k, 1, DownWholeSizes);
	FO3DAudioResampler DownChunked;
	DownChunked.Configure(48000, 16000, 1);
	const TArray<float> DownChunkedOut = ResampleInChunks(DownChunked, Mono48k, 1, IrregularSizes);
	TestEqual(TEXT("48 kHz to 16 kHz: exact length"), DownWholeOut.Num(), 32000);
	TestTrue(TEXT("48 kHz to 16 kHz: chunking does not change the output"), DownChunkedOut == DownWholeOut);

	// The reported offset of each buffer's first output frame stays within one input frame.
	FO3DAudioResampler Offsets;
	Offsets.Configure(InRate, OutRate, 1);
	const TArray<float> Silence = MakeSine(441, 1, InRate, 0.0);
	TArray<float> Out;
	for (int32 Buffer = 0; Buffer < 8; ++Buffer)
	{
		double Offset = 0.0;
		const int32 Frames = Offsets.Process(Silence.GetData(), 441, Out, Offset);
		TestTrue(TEXT("479 or 480 frames per 10 ms buffer"), Frames == 479 || Frames == 480);
		TestTrue(TEXT("First-frame offset within one input frame"), Offset > -1.0 / InRate && Offset < 1.0 / OutRate);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioResamplerAntiAliasTest, "Open3DBroadcast.Shared.Audio.Resampler.AntiAliasing", O3DB_TEST_FLAGS)
bool FO3DAudioResamplerAntiAliasTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioResamplerTests;

	// 48 kHz to 16 kHz (8 kHz Nyquist). A 1 kHz tone passes; a 12 kHz tone would alias to 4 kHz
	// at full level without filtering (RMS 0.707); with the filter it is at least 30 dB down.
	const int32 Sizes[] = { 480 };
	const int32 Skip = 1600; // 100 ms of filter settling

	FO3DAudioResampler Pass;
	Pass.Configure(48000, 16000, 1);
	const double PassRms = Rms(ResampleInChunks(Pass, MakeSine(48000, 1, 48000, 1000.0), 1, Sizes), Skip);
	TestTrue(FString::Printf(TEXT("1 kHz passes (RMS %.4f)"), PassRms), FMath::Abs(PassRms - 0.70710678) < 0.01);

	FO3DAudioResampler Stop;
	Stop.Configure(48000, 16000, 1);
	const double StopRms = Rms(ResampleInChunks(Stop, MakeSine(48000, 1, 48000, 12000.0), 1, Sizes), Skip);
	TestTrue(FString::Printf(TEXT("12 kHz is attenuated (RMS %.4f)"), StopRms), StopRms < 0.02);

	FO3DAudioResampler Invalid;
	TestFalse(TEXT("A zero rate is rejected"), Invalid.Configure(0, 48000, 1));
	TArray<float> Out;
	double Offset = 0.0;
	const float One = 1.0f;
	TestEqual(TEXT("An unconfigured resampler produces nothing"), Invalid.Process(&One, 1, Out, Offset), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
