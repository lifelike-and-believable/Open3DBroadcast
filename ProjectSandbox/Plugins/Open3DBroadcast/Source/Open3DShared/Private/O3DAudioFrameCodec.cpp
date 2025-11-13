#include "O3DAudioFrameCodec.h"

DEFINE_LOG_CATEGORY(LogO3DAudioCodec);

namespace
{
	inline int16 FloatToPcm16(float Sample)
	{
		const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
		const int32 Scaled = FMath::RoundToInt(Clamped * 32767.0f);
		return static_cast<int16>(FMath::Clamp(Scaled, -32768, 32767));
	}
}

namespace O3DAudio
{
	FString SanitizeCodecString(const FString& InCodec)
	{
		FString Result = InCodec;
		Result.TrimStartAndEndInline();
		Result.ToLowerInline();
		return Result;
	}

	O3DS::EUnifiedCodec SelectCodec(const FO3DTransportAudioConfig& Config)
	{
		FString CodecString = SanitizeCodecString(Config.Codec);
		if (CodecString.IsEmpty())
		{
			if (const FString* Override = Config.AdvancedParams.Find(TEXT("codec")))
			{
				CodecString = SanitizeCodecString(*Override);
			}
		}

		if (CodecString == TEXT("opus"))
		{
			return O3DS::EUnifiedCodec::Opus;
		}

		return O3DS::EUnifiedCodec::PCM16;
	}

	bool FFrameEncoder::Initialize(const FO3DTransportAudioConfig& Config, const FString& InDefaultStreamLabel, const FString& InDefaultSubject)
	{
		AudioConfig = Config;
		DefaultStreamLabel = InDefaultStreamLabel;
		DefaultSubject = InDefaultSubject;

		if (DefaultStreamLabel.IsEmpty())
		{
			DefaultStreamLabel = DefaultSubject;
		}

		if (AudioConfig.StreamLabel.IsEmpty())
		{
			AudioConfig.StreamLabel = DefaultStreamLabel;
		}

		ActiveCodec = SelectCodec(AudioConfig);
		bInitialized = true;
		bOpusReady = false;
		PCM16Scratch.Reset();
		return true;
	}

	bool FFrameEncoder::EnsureOpusEncoder(int32 SampleRate, int32 NumChannels)
	{
#if !O3D_WITH_OPUS
		return false;
#else
		if (ActiveCodec != O3DS::EUnifiedCodec::Opus)
		{
			return false;
		}

		const int32 TargetSampleRate = SampleRate > 0 ? SampleRate : AudioConfig.SampleRate;
		const int32 TargetChannels = NumChannels > 0 ? NumChannels : AudioConfig.NumChannels;

		if (TargetSampleRate <= 0 || TargetChannels <= 0)
		{
			return false;
		}

		const bool bNeedsReinitialise = !bOpusReady
			|| OpusEncoder.GetSettings().SampleRate != TargetSampleRate
			|| OpusEncoder.GetSettings().NumChannels != TargetChannels;

		if (!bNeedsReinitialise)
		{
			return bOpusReady;
		}

		FString Error;
		FO3DAudioOpusEncoder::FSettings Settings;
		Settings.SampleRate = TargetSampleRate;
		Settings.NumChannels = TargetChannels;
		Settings.BitrateKbps = AudioConfig.BitrateKbps;
		Settings.bUseVariableBitrate = true;

		if (!OpusEncoder.Initialize(Settings, Error))
		{
			UE_LOG(LogO3DAudioCodec, Warning, TEXT("Opus encoder initialisation failed (%s); falling back to PCM16."), *Error);
			bOpusReady = false;
			ActiveCodec = O3DS::EUnifiedCodec::PCM16;
			return false;
		}

		bOpusReady = true;
		return true;
#endif
	}

	FString FFrameEncoder::ResolveStreamLabel(const FString& Override) const
	{
		if (!Override.IsEmpty())
		{
			return Override;
		}
		if (!AudioConfig.StreamLabel.IsEmpty())
		{
			return AudioConfig.StreamLabel;
		}
		return DefaultStreamLabel;
	}

	FString FFrameEncoder::ResolveSubject(const FString& Override) const
	{
		if (!Override.IsEmpty())
		{
			return Override;
		}
		return DefaultSubject;
	}

	bool FFrameEncoder::BuildEncodedFrame(const FString& StreamLabelOverride,
		const FString& SubjectOverride,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec,
		FEncodedFrame& OutFrame)
	{
		if (!bInitialized || !Interleaved || NumFrames <= 0)
		{
			return false;
		}

		const int32 EffectiveChannels = NumChannels > 0 ? NumChannels : FMath::Max(AudioConfig.NumChannels, 1);
		const int32 EffectiveSampleRate = SampleRate > 0 ? SampleRate : FMath::Max(AudioConfig.SampleRate, 1);
		const int32 NumSamples = EffectiveChannels * NumFrames;
		if (NumSamples <= 0)
		{
			return false;
		}

		FEncodedFrame Frame;
		Frame.Codec = ActiveCodec;
		Frame.Meta.StreamLabel = ResolveStreamLabel(StreamLabelOverride);
		Frame.Meta.SubjectName = ResolveSubject(SubjectOverride);
		Frame.Meta.SampleRate = EffectiveSampleRate;
		Frame.Meta.NumChannels = EffectiveChannels;
		Frame.Meta.TimestampSec = TimestampSec;

		if (ActiveCodec == O3DS::EUnifiedCodec::Opus && EnsureOpusEncoder(EffectiveSampleRate, EffectiveChannels))
		{
#if O3D_WITH_OPUS
			TArray<uint8> Encoded;
			int32 FramesEncoded = 0;
			if (OpusEncoder.Encode(Interleaved, NumFrames, Encoded, FramesEncoded) && Encoded.Num() > 0)
			{
				Frame.Encoded = MoveTemp(Encoded);
				OutFrame = MoveTemp(Frame);
				return true;
			}
			else
			{
				UE_LOG(LogO3DAudioCodec, Verbose, TEXT("Opus encode failure; reverting to PCM16 for this frame."));
				ActiveCodec = O3DS::EUnifiedCodec::PCM16;
				bOpusReady = false;
				Frame.Codec = O3DS::EUnifiedCodec::PCM16;
			}
#endif
		}

		// PCM16 path (default or Opus fallback)
		PCM16Scratch.SetNumUninitialized(NumSamples);
		for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
		{
			PCM16Scratch[SampleIndex] = FloatToPcm16(Interleaved[SampleIndex]);
		}

		Frame.Encoded.SetNumUninitialized(NumSamples * sizeof(int16));
		FMemory::Memcpy(Frame.Encoded.GetData(), PCM16Scratch.GetData(), Frame.Encoded.Num());

		OutFrame = MoveTemp(Frame);
		return true;
	}

	bool FFrameDecoder::EnsureOpusDecoder(int32 SampleRate, int32 NumChannels)
	{
#if !O3D_WITH_OPUS
		return false;
#else
		const int32 TargetSampleRate = SampleRate > 0 ? SampleRate : 48000;
		const int32 TargetChannels = NumChannels > 0 ? NumChannels : 1;

		const bool bNeedsReinitialise = !bOpusReady
			|| CachedSampleRate != TargetSampleRate
			|| CachedNumChannels != TargetChannels;

		if (!bNeedsReinitialise)
		{
			return bOpusReady;
		}

		FString Error;
		FO3DAudioOpusDecoder::FSettings Settings;
		Settings.SampleRate = TargetSampleRate;
		Settings.NumChannels = TargetChannels;

		if (!OpusDecoder.Initialize(Settings, Error))
		{
			UE_LOG(LogO3DAudioCodec, Warning, TEXT("Opus decoder initialisation failed (%s)."), *Error);
			bOpusReady = false;
			return false;
		}

		CachedSampleRate = TargetSampleRate;
		CachedNumChannels = TargetChannels;
		bOpusReady = true;
		return true;
#endif
	}

	bool FFrameDecoder::Decode(O3DS::EUnifiedCodec Codec,
		const O3DS::FAudioFrameMeta& Meta,
		const uint8* Payload,
		int32 PayloadSize,
		TArray<int16>& OutPcm16)
	{
		if (!Payload || PayloadSize <= 0)
		{
			return false;
		}

		if (Codec == O3DS::EUnifiedCodec::PCM16)
		{
			if ((PayloadSize % static_cast<int32>(sizeof(int16))) != 0)
			{
				return false;
			}

			const int32 NumSamples = PayloadSize / static_cast<int32>(sizeof(int16));
			OutPcm16.SetNumUninitialized(NumSamples);
			FMemory::Memcpy(OutPcm16.GetData(), Payload, PayloadSize);
			return true;
		}

		if (Codec == O3DS::EUnifiedCodec::Opus)
		{
#if O3D_WITH_OPUS
			if (!EnsureOpusDecoder(Meta.SampleRate, Meta.NumChannels))
			{
				return false;
			}

			int32 FramesDecoded = 0;
			if (!OpusDecoder.Decode(Payload, PayloadSize, OutPcm16, FramesDecoded))
			{
				return false;
			}

			// Resize already handled by decoder.
			return true;
#else
			return false;
#endif
		}

		return false;
	}

	bool SerializeForTransport(const FEncodedFrame& Frame, TArray<uint8>& OutPayload)
	{
		if (Frame.Encoded.Num() <= 0)
		{
			return false;
		}

		return SerializeEncodedAudioFrame(Frame.Codec, Frame.Meta, Frame.Encoded.GetData(), Frame.Encoded.Num(), OutPayload);
	}

	bool CreateUnifiedAudioMessage(const FEncodedFrame& Frame, double TimestampSec, TArray<uint8>& OutMessage)
	{
		TArray<uint8> Payload;
		if (!SerializeForTransport(Frame, Payload))
		{
			return false;
		}

		return O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Audio, Frame.Codec, Payload.GetData(), Payload.Num(), TimestampSec, OutMessage);
	}
}
