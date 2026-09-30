#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioOpus.h"

#if O3D_WITH_OPUS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioOpusRoundTripTest, "Open3DBroadcast.O3DShared.Audio.Opus.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DAudioOpusRoundTripTest::RunTest(const FString& Parameters)
{
	FO3DAudioOpusEncoder::FSettings EncoderSettings;
	EncoderSettings.SampleRate = 48000;
	EncoderSettings.NumChannels = 1;
	EncoderSettings.FrameSizeMs = 20;
	EncoderSettings.BitrateKbps = 64;

	FO3DAudioOpusEncoder Encoder;
	FString Error;
	TestTrue(TEXT("Encoder initialization should succeed"), Encoder.Initialize(EncoderSettings, Error));
	if (!Encoder.IsInitialized())
	{
		AddError(FString::Printf(TEXT("Encoder Initialize failed: %s"), *Error));
		return false;
	}

	const int32 FrameSamples = (EncoderSettings.SampleRate / 1000) * EncoderSettings.FrameSizeMs;
	// Encode multiple frames to test codec state handling
	const int32 NumFramesToEncode = 5;
	const int32 TotalInputSamples = FrameSamples * NumFramesToEncode;
	TArray<float> Input;
	Input.SetNumZeroed(TotalInputSamples);

	// Generate a continuous sine wave across multiple frames
	for (int32 Index = 0; Index < TotalInputSamples; ++Index)
	{
		const float Phase = static_cast<float>(Index) / static_cast<float>(TotalInputSamples);
		Input[Index] = FMath::Sin(Phase * 2.0f * PI);
	}

	// Encode frame-by-frame (Opus encoder requires frames matching FrameSizeMs)
	// Track individual frame packets to decode them separately
	TArray<TArray<uint8>> EncodedFrames;
	int32 TotalFramesEncoded = 0;

	for (int32 FrameIdx = 0; FrameIdx < NumFramesToEncode; ++FrameIdx)
	{
		const float* FrameData = &Input[FrameIdx * FrameSamples];
		TArray<uint8> FrameEncoded;
		int32 FramesEncoded = 0;

		if (!Encoder.Encode(FrameData, FrameSamples, FrameEncoded, FramesEncoded))
		{
			AddError(FString::Printf(TEXT("Failed to encode frame %d"), FrameIdx));
			return false;
		}

		if (FramesEncoded != FrameSamples)
		{
			AddError(FString::Printf(TEXT("Frame %d: Expected %d frames encoded, got %d"), FrameIdx, FrameSamples, FramesEncoded));
			return false;
		}

		EncodedFrames.Add(FrameEncoded);
		TotalFramesEncoded += FramesEncoded;
	}

	TestTrue(TEXT("Encode should produce payload"), EncodedFrames.Num() > 0);
	TestEqual(TEXT("Total frames encoded should match input"), TotalFramesEncoded, TotalInputSamples);

	UE_LOG(LogTemp, Log, TEXT("Encode successful: %d samples encoded into %d separate Opus packets across %d frames"),
		TotalFramesEncoded, EncodedFrames.Num(), NumFramesToEncode);
	for (int32 i = 0; i < EncodedFrames.Num(); ++i)
	{
UE_LOG(LogTemp, Log, TEXT("  Frame %d: %d bytes"), i, EncodedFrames[i].Num());
	}

	// Debug: Log input audio samples to understand what we're encoding
	float InputMin = 0.0f, InputMax = 0.0f, InputRms = 0.0f;
	for (int32 i = 0; i < TotalInputSamples; ++i)
	{
		InputMin = FMath::Min(InputMin, Input[i]);
		InputMax = FMath::Max(InputMax, Input[i]);
		InputRms += Input[i] * Input[i];
	}
	InputRms = FMath::Sqrt(InputRms / TotalInputSamples);

	UE_LOG(LogTemp, Log, TEXT("Input signal: Min=%.6f, Max=%.6f, RMS=%.6f"), InputMin, InputMax, InputRms);
	UE_LOG(LogTemp, Log, TEXT("Input audio samples (first 10):"));
	for (int32 i = 0; i < FMath::Min(10, TotalInputSamples); ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("  Input[%d] = %.6f"), i, Input[i]);
	}
	UE_LOG(LogTemp, Log, TEXT("Input audio samples (around peak, near sample 2880):"));
	for (int32 i = 2875; i < 2885 && i < TotalInputSamples; ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("  Input[%d] = %.6f"), i, Input[i]);
	}

	FO3DAudioOpusDecoder::FSettings DecoderSettings;
	DecoderSettings.SampleRate = EncoderSettings.SampleRate;
	DecoderSettings.NumChannels = EncoderSettings.NumChannels;
	DecoderSettings.FrameSizeMs = EncoderSettings.FrameSizeMs;

	FO3DAudioOpusDecoder Decoder;
	TestTrue(TEXT("Decoder initialization should succeed"), Decoder.Initialize(DecoderSettings, Error));
	if (!Decoder.IsInitialized())
	{
		AddError(FString::Printf(TEXT("Decoder Initialize failed: %s"), *Error));
		return false;
	}

	// Decode frame-by-frame, accumulating results
	TArray<int16> Decoded;
	int32 TotalFramesDecoded = 0;

	for (int32 FrameIdx = 0; FrameIdx < EncodedFrames.Num(); ++FrameIdx)
	{
		TArray<int16> FrameDecoded;
		int32 FramesDecoded = 0;

		if (!Decoder.Decode(EncodedFrames[FrameIdx].GetData(), EncodedFrames[FrameIdx].Num(), FrameDecoded, FramesDecoded))
		{
			AddError(FString::Printf(TEXT("Failed to decode frame %d (input: %d bytes)"), FrameIdx, EncodedFrames[FrameIdx].Num()));
			return false;
		}

		if (FramesDecoded <= 0)
		{
			AddError(FString::Printf(TEXT("Frame %d decode returned 0 frames"), FrameIdx));
			return false;
		}

		UE_LOG(LogTemp, Log, TEXT("Decoded frame %d: %d bytes -> %d samples"), FrameIdx, EncodedFrames[FrameIdx].Num(), FrameDecoded.Num());
		Decoded.Append(FrameDecoded);
		TotalFramesDecoded += FramesDecoded;
	}

	TestTrue(TEXT("Decoded PCM should not be empty"), Decoded.Num() > 0);
	if (Decoded.Num() == 0)
	{
		AddError(TEXT("Decoded array is empty"));
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("Total decode: %d frames, %d samples"), TotalFramesDecoded, Decoded.Num());

	// Calculate samples per channel: use actual decoded frames, not array size
	// (array might be larger due to decoder buffer allocation)
	const int32 NumChannels = DecoderSettings.NumChannels;
	const int32 SamplesPerChannel = TotalFramesDecoded;

	const FString DiagnosticLog = FString::Printf(TEXT("\n=== Opus Roundtrip Test Diagnostics ===\n")
		TEXT("Encoder settings: SampleRate=%d, NumChannels=%d, FrameSizeMs=%d, BitrateKbps=%d\n")
		TEXT("Input: %d total samples (%d frames of %d samples each)\n")
		TEXT("Total encoded packets: %d\n")
		TEXT("Decoded: %d total samples, %d frames decoded\n"),
		EncoderSettings.SampleRate, EncoderSettings.NumChannels, EncoderSettings.FrameSizeMs, EncoderSettings.BitrateKbps,
		TotalInputSamples, NumFramesToEncode, FrameSamples, EncodedFrames.Num(), Decoded.Num(), TotalFramesDecoded);

	UE_LOG(LogTemp, Log, TEXT("%s"), *DiagnosticLog);

	if (TotalFramesDecoded != TotalInputSamples)
	{
		AddWarning(FString::Printf(TEXT("Frame count mismatch: Expected %d frames, got %d frames"), TotalInputSamples, TotalFramesDecoded));
	}

	if (SamplesPerChannel < TotalInputSamples)
	{
		AddError(FString::Printf(TEXT("Decoded sample count (%d) less than input (%d)"), SamplesPerChannel, TotalInputSamples));
		return false;
	}

	float AccumulatedDifference = 0.0f;
	float MaxDifference = 0.0f;
	int32 MaxDifferenceIndex = 0;
	const int32 CompareSamples = FMath::Min(SamplesPerChannel, TotalInputSamples);

	// Debug: Log first few and worst samples
	float FirstSampleDebug = static_cast<float>(Decoded[0]) / 32767.0f;
	UE_LOG(LogTemp, Log, TEXT("Sample comparison debug: Input[0]=%.6f, Decoded[0]=%d, Reconstructed[0]=%.6f"),
		Input[0], Decoded[0], FirstSampleDebug);

	for (int32 SampleIndex = 0; SampleIndex < CompareSamples; ++SampleIndex)
	{
		const float Original = Input[SampleIndex];
		const float Reconstructed = static_cast<float>(Decoded[SampleIndex]) / 32767.0f;
		const float Difference = FMath::Abs(Original - Reconstructed);
		AccumulatedDifference += Difference;

		if (Difference > MaxDifference)
		{
			MaxDifference = Difference;
			MaxDifferenceIndex = SampleIndex;
		}
	}

	if (MaxDifferenceIndex > 0)
	{
		float WorstOriginal = Input[MaxDifferenceIndex];
		int16 WorstDecoded = Decoded[MaxDifferenceIndex];
		float WorstReconstructed = static_cast<float>(WorstDecoded) / 32767.0f;
		UE_LOG(LogTemp, Log, TEXT("Worst sample [%d]: Input=%.6f, Decoded=%d, Reconstructed=%.6f, Diff=%.6f"),
			MaxDifferenceIndex, WorstOriginal, WorstDecoded, WorstReconstructed, MaxDifference);
	}

	const float AverageDifference = AccumulatedDifference / static_cast<float>(CompareSamples);

	const FString ErrorLog = FString::Printf(TEXT("Reconstruction error: Average=%.6f, Max=%.6f (at sample %d)"),
		AverageDifference, MaxDifference, MaxDifferenceIndex);
		UE_LOG(LogTemp, Log, TEXT("%s"), *ErrorLog);

	// Opus is lossy compression; 0.15f is a reasonable tolerance for 64kbps mono audio
	if (AverageDifference >= 0.15f)
	{
		AddError(FString::Printf(TEXT("Average reconstruction error (%.6f) exceeds threshold (0.15)"), AverageDifference));
		return false;
	}

	return true;
}

#endif // O3D_WITH_OPUS

#endif // WITH_DEV_AUTOMATION_TESTS
