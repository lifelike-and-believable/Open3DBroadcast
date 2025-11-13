#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioOpus.h"

#if O3D_WITH_OPUS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioOpusRoundTripTest, "Open3DShared.Audio.Opus.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
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

	const int32 NumFrames = (EncoderSettings.SampleRate / 1000) * EncoderSettings.FrameSizeMs;
	TArray<float> Input;
	Input.SetNumZeroed(NumFrames);
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const float Phase = static_cast<float>(Index) / static_cast<float>(NumFrames);
		Input[Index] = FMath::Sin(Phase * 2.0f * PI);
	}

	TArray<uint8> Encoded;
	int32 FramesEncoded = 0;
	TestTrue(TEXT("Encode should succeed"), Encoder.Encode(Input.GetData(), NumFrames, Encoded, FramesEncoded));
	TestTrue(TEXT("Encode should produce payload"), Encoded.Num() > 0);
	TestEqual(TEXT("Frames encoded should match input"), FramesEncoded, NumFrames);

	FO3DAudioOpusDecoder::FSettings DecoderSettings;
	DecoderSettings.SampleRate = EncoderSettings.SampleRate;
	DecoderSettings.NumChannels = EncoderSettings.NumChannels;
	DecoderSettings.FrameSizeMs = 60;

	FO3DAudioOpusDecoder Decoder;
	TestTrue(TEXT("Decoder initialization should succeed"), Decoder.Initialize(DecoderSettings, Error));
	if (!Decoder.IsInitialized())
	{
		AddError(FString::Printf(TEXT("Decoder Initialize failed: %s"), *Error));
		return false;
	}

	TArray<int16> Decoded;
	int32 FramesDecoded = 0;
	TestTrue(TEXT("Decode should succeed"), Decoder.Decode(Encoded.GetData(), Encoded.Num(), Decoded, FramesDecoded));
	TestTrue(TEXT("Decoded PCM should not be empty"), Decoded.Num() > 0);
	TestTrue(TEXT("Decoded frames should be positive"), FramesDecoded > 0);

	const int32 SamplesPerChannel = Decoded.Num();
	float AccumulatedDifference = 0.0f;
	const int32 CompareSamples = FMath::Min(SamplesPerChannel, NumFrames);
	for (int32 SampleIndex = 0; SampleIndex < CompareSamples; ++SampleIndex)
	{
		const float Original = Input[SampleIndex];
		const float Reconstructed = static_cast<float>(Decoded[SampleIndex]) / 32767.0f;
		AccumulatedDifference += FMath::Abs(Original - Reconstructed);
	}

	const float AverageDifference = AccumulatedDifference / static_cast<float>(CompareSamples);
	TestTrue(TEXT("Average reconstruction error should be reasonable"), AverageDifference < 0.2f);

	return true;
}

#endif // O3D_WITH_OPUS

#endif // WITH_DEV_AUTOMATION_TESTS
