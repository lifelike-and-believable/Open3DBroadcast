#include "O3DAudioOpus.h"

#include "Logging/LogMacros.h"

#if O3D_WITH_OPUS
#include "opus.h"
#endif

namespace
{
#if O3D_WITH_OPUS
	int32 CalculateMaxFrameSamples(const FO3DAudioOpusDecoder::FSettings& Settings)
	{
		// Opus packets carry at most 120 ms (RFC 6716 section 3.2.5).
		const int32 ClampedFrameMs = FMath::Clamp(Settings.FrameSizeMs, 10, 120);
		return (Settings.SampleRate / 1000) * ClampedFrameMs;
	}
#endif
}

bool FO3DAudioOpusEncoder::IsSupportedSampleRate(int32 SampleRate)
{
	return SampleRate == 8000 || SampleRate == 12000 || SampleRate == 16000 || SampleRate == 24000 || SampleRate == 48000;
}

bool FO3DAudioOpusEncoder::IsSupportedFrameSizeMs(int32 FrameSizeMs)
{
	// 2.5 ms is also legal Opus but cannot be expressed in whole milliseconds.
	return FrameSizeMs == 5 || FrameSizeMs == 10 || FrameSizeMs == 20 || FrameSizeMs == 40 || FrameSizeMs == 60;
}

FO3DAudioOpusEncoder::FO3DAudioOpusEncoder() = default;
FO3DAudioOpusEncoder::~FO3DAudioOpusEncoder()
{
	Reset();
}

bool FO3DAudioOpusEncoder::Initialize(const FSettings& InSettings, FString& OutError)
{
	Reset();
	Settings = InSettings;

#if !O3D_WITH_OPUS
	OutError = TEXT("Opus support disabled at build time.");
	return false;
#else
	if (!IsSupportedSampleRate(Settings.SampleRate))
	{
		OutError = FString::Printf(TEXT("Opus cannot encode at %d Hz (use 8000, 12000, 16000, 24000 or 48000)."), Settings.SampleRate);
		return false;
	}
	if (Settings.NumChannels < 1 || Settings.NumChannels > 2)
	{
		OutError = FString::Printf(TEXT("Opus encodes 1 or 2 channels, not %d."), Settings.NumChannels);
		return false;
	}
	if (!IsSupportedFrameSizeMs(Settings.FrameSizeMs))
	{
		OutError = FString::Printf(TEXT("%d ms is not an Opus frame duration (use 5, 10, 20, 40 or 60)."), Settings.FrameSizeMs);
		return false;
	}

	int Error = 0;
	Encoder = opus_encoder_create(Settings.SampleRate, Settings.NumChannels, OPUS_APPLICATION_AUDIO, &Error);
	if (Error != OPUS_OK || !Encoder)
	{
		OutError = FString::Printf(TEXT("opus_encoder_create failed (%d)"), Error);
		Encoder = nullptr;
		return false;
	}

	// SHR-31: a rejected ctl is an initialisation failure, not a silent default.
	const opus_int32 BitrateBps = Settings.BitrateKbps > 0 ? static_cast<opus_int32>(Settings.BitrateKbps) * 1000 : OPUS_AUTO;
	int CtlResult = opus_encoder_ctl(Encoder, OPUS_SET_BITRATE(BitrateBps));
	if (CtlResult != OPUS_OK)
	{
		OutError = FString::Printf(TEXT("OPUS_SET_BITRATE(%d) failed (%d)"), static_cast<int32>(BitrateBps), CtlResult);
		Reset();
		return false;
	}
	CtlResult = opus_encoder_ctl(Encoder, OPUS_SET_VBR(Settings.bUseVariableBitrate ? 1 : 0));
	if (CtlResult != OPUS_OK)
	{
		OutError = FString::Printf(TEXT("OPUS_SET_VBR failed (%d)"), CtlResult);
		Reset();
		return false;
	}
	CtlResult = opus_encoder_ctl(Encoder, OPUS_SET_COMPLEXITY(FMath::Clamp(Settings.Complexity, 0, 10)));
	if (CtlResult != OPUS_OK)
	{
		OutError = FString::Printf(TEXT("OPUS_SET_COMPLEXITY failed (%d)"), CtlResult);
		Reset();
		return false;
	}
	opus_int32 Lookahead = 0;
	CtlResult = opus_encoder_ctl(Encoder, OPUS_GET_LOOKAHEAD(&Lookahead));
	if (CtlResult != OPUS_OK)
	{
		OutError = FString::Printf(TEXT("OPUS_GET_LOOKAHEAD failed (%d)"), CtlResult);
		Reset();
		return false;
	}

	LookaheadSamples = static_cast<int32>(Lookahead);
	FrameSizeSamples = Settings.SampleRate / 1000 * Settings.FrameSizeMs;
	return true;
#endif
}

void FO3DAudioOpusEncoder::Reset()
{
#if O3D_WITH_OPUS
	if (Encoder)
	{
		opus_encoder_destroy(Encoder);
		Encoder = nullptr;
	}
#else
	Encoder = nullptr;
#endif
	FrameSizeSamples = 0;
	LookaheadSamples = 0;
}

bool FO3DAudioOpusEncoder::Encode(const float* InterleavedPCM, int32 NumFrames, TArray<uint8>& OutPayload, int32& OutFramesEncoded)
{
	OutFramesEncoded = 0;

#if !O3D_WITH_OPUS
	return false;
#else
	if (!Encoder || !InterleavedPCM || NumFrames <= 0 || NumFrames != FrameSizeSamples)
	{
		return false;
	}

	OutPayload.SetNumUninitialized(MaxPacketBytes, EAllowShrinking::No);

	const int EncodedBytes = opus_encode_float(Encoder,
		InterleavedPCM,
		NumFrames,
		reinterpret_cast<unsigned char*>(OutPayload.GetData()),
		MaxPacketBytes);

	if (EncodedBytes < 0)
	{
		OutPayload.SetNum(0, EAllowShrinking::No);
		return false;
	}

	OutFramesEncoded = NumFrames;
	OutPayload.SetNum(EncodedBytes, EAllowShrinking::No);
	return true;
#endif
}

FO3DAudioOpusDecoder::FO3DAudioOpusDecoder() = default;
FO3DAudioOpusDecoder::~FO3DAudioOpusDecoder()
{
	Reset();
}

bool FO3DAudioOpusDecoder::Initialize(const FSettings& InSettings, FString& OutError)
{
	Reset();
	Settings = InSettings;

#if !O3D_WITH_OPUS
	OutError = TEXT("Opus support disabled at build time.");
	return false;
#else
	if (!FO3DAudioOpusEncoder::IsSupportedSampleRate(Settings.SampleRate) || Settings.NumChannels < 1 || Settings.NumChannels > 2)
	{
		OutError = FString::Printf(TEXT("Invalid Opus decoder settings (%d Hz, %d channels)."), Settings.SampleRate, Settings.NumChannels);
		return false;
	}

	int Error = 0;
	Decoder = opus_decoder_create(Settings.SampleRate, Settings.NumChannels, &Error);
	if (Error != OPUS_OK || !Decoder)
	{
		OutError = FString::Printf(TEXT("opus_decoder_create failed (%d)"), Error);
		Decoder = nullptr;
		return false;
	}

	MaxFrameSizeSamples = CalculateMaxFrameSamples(Settings);
	return true;
#endif
}

void FO3DAudioOpusDecoder::Reset()
{
#if O3D_WITH_OPUS
	if (Decoder)
	{
		opus_decoder_destroy(Decoder);
		Decoder = nullptr;
	}
#else
	Decoder = nullptr;
#endif
	MaxFrameSizeSamples = 0;
}

bool FO3DAudioOpusDecoder::Decode(const uint8* EncodedData, int32 NumBytes, TArray<int16>& OutPcm16, int32& OutFramesDecoded)
{
	OutFramesDecoded = 0;

#if !O3D_WITH_OPUS
	return false;
#else
	if (!Decoder || !EncodedData || NumBytes <= 0)
	{
		return false;
	}

	const int32 Channels = Settings.NumChannels;
	const int32 FrameCapacity = MaxFrameSizeSamples;
	if (FrameCapacity <= 0)
	{
		return false;
	}

	OutPcm16.SetNumUninitialized(FrameCapacity * Channels, EAllowShrinking::No);

	const int DecodedFrames = opus_decode(Decoder,
		reinterpret_cast<const unsigned char*>(EncodedData),
		NumBytes,
		reinterpret_cast<opus_int16*>(OutPcm16.GetData()),
		FrameCapacity,
		Settings.bEnableFec ? 1 : 0);

	if (DecodedFrames < 0)
	{
		OutPcm16.SetNum(0, EAllowShrinking::No);
		return false;
	}

	OutFramesDecoded = DecodedFrames;
	OutPcm16.SetNum(DecodedFrames * Channels, EAllowShrinking::No);
	return true;
#endif
}

bool FO3DAudioOpusDecoder::DecodeLost(int32 NumFrames, TArray<int16>& OutPcm16, int32& OutFramesDecoded)
{
	OutFramesDecoded = 0;

#if !O3D_WITH_OPUS
	return false;
#else
	if (!Decoder || NumFrames <= 0 || NumFrames > MaxFrameSizeSamples)
	{
		return false;
	}

	const int32 Channels = Settings.NumChannels;
	OutPcm16.SetNumUninitialized(NumFrames * Channels, EAllowShrinking::No);

	// A null packet asks libOpus for concealment of NumFrames frames.
	const int DecodedFrames = opus_decode(Decoder,
		nullptr,
		0,
		reinterpret_cast<opus_int16*>(OutPcm16.GetData()),
		NumFrames,
		0);

	if (DecodedFrames < 0)
	{
		OutPcm16.SetNum(0, EAllowShrinking::No);
		return false;
	}

	OutFramesDecoded = DecodedFrames;
	OutPcm16.SetNum(DecodedFrames * Channels, EAllowShrinking::No);
	return true;
#endif
}
