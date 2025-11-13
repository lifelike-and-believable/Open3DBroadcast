#include "O3DAudioOpus.h"

#include "Logging/LogMacros.h"

#if O3D_WITH_OPUS
#include "opus.h"
#endif

namespace
{
#if O3D_WITH_OPUS
	int32 CalculateMaxPacketBytes(const FO3DAudioOpusEncoder::FSettings& Settings)
	{
		const int32 FrameSizeSamples = (Settings.SampleRate / 1000) * Settings.FrameSizeMs;
		// 1275 bytes per RFC6716 section 5.1, plus small overhead margin.
		const int32 Base = 1275;
		const int32 Safety = 8;
		return FMath::Max(Base + Safety, (FrameSizeSamples * Settings.NumChannels) / 4);
	}

	int32 CalculateMaxFrameSamples(const FO3DAudioOpusDecoder::FSettings& Settings)
	{
		const int32 ClampedFrameMs = FMath::Clamp(Settings.FrameSizeMs, 10, 120);
		return (Settings.SampleRate / 1000) * ClampedFrameMs;
	}
#endif
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
	if (Settings.SampleRate <= 0 || Settings.NumChannels <= 0)
	{
		OutError = TEXT("Invalid Opus encoder settings.");
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

	const opus_int32 BitrateBps = Settings.BitrateKbps > 0 ? static_cast<opus_int32>(Settings.BitrateKbps * 1000) : OPUS_AUTO;
	opus_encoder_ctl(Encoder, OPUS_SET_BITRATE(BitrateBps));
	opus_encoder_ctl(Encoder, OPUS_SET_VBR(Settings.bUseVariableBitrate ? 1 : 0));
	opus_encoder_ctl(Encoder, OPUS_SET_COMPLEXITY(FMath::Clamp(Settings.Complexity, 0, 10)));

	MaxPacketBytes = CalculateMaxPacketBytes(Settings);
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
	MaxPacketBytes = 0;
}

bool FO3DAudioOpusEncoder::Encode(const float* InterleavedPCM, int32 NumFrames, TArray<uint8>& OutPayload, int32& OutFramesEncoded)
{
	OutFramesEncoded = 0;

#if !O3D_WITH_OPUS
	return false;
#else
	if (!Encoder || !InterleavedPCM || NumFrames <= 0)
	{
		return false;
	}

	OutPayload.SetNumUninitialized(MaxPacketBytes);

	const int EncodedBytes = opus_encode_float(Encoder,
		InterleavedPCM,
		NumFrames,
		reinterpret_cast<unsigned char*>(OutPayload.GetData()),
		MaxPacketBytes);

	if (EncodedBytes < 0)
	{
		OutPayload.Reset();
		return false;
	}

	OutFramesEncoded = NumFrames;
	OutPayload.SetNum(EncodedBytes, EAllowShrinking::Yes);
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
	if (Settings.SampleRate <= 0 || Settings.NumChannels <= 0)
	{
		OutError = TEXT("Invalid Opus decoder settings.");
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

	OutPcm16.SetNumUninitialized(FrameCapacity * Channels);

	const int DecodedFrames = opus_decode(Decoder,
		reinterpret_cast<const unsigned char*>(EncodedData),
		NumBytes,
		reinterpret_cast<opus_int16*>(OutPcm16.GetData()),
		FrameCapacity,
		Settings.bEnableFec ? 1 : 0);

	if (DecodedFrames < 0)
	{
		OutPcm16.Reset();
		return false;
	}

	OutFramesDecoded = DecodedFrames;
	OutPcm16.SetNum(DecodedFrames * Channels, EAllowShrinking::Yes);
	return true;
#endif
}
