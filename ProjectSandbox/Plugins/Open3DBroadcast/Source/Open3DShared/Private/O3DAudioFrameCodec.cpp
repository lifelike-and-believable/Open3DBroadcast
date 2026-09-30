#include "O3DAudioFrameCodec.h"

#include "HAL/PlatformTime.h"

#include <atomic>

DEFINE_LOG_CATEGORY(LogO3DAudioCodec);

namespace
{
	inline int16 FloatToPcm16(float Sample)
	{
		const float Clamped = FMath::Clamp(Sample, -1.0f, 1.0f);
		const int32 Scaled = FMath::RoundToInt(Clamped * 32767.0f);
		return static_cast<int16>(FMath::Clamp(Scaled, -32768, 32767));
	}

	/** Process-wide throttle for Opus warnings, so a failing stream cannot flood the log. */
	bool ShouldLogOpusWarning()
	{
		static std::atomic<double> LastLogTime{-1000.0};
		const double Now = FPlatformTime::Seconds();
		double Last = LastLogTime.load();
		return Now - Last > 5.0 && LastLogTime.compare_exchange_strong(Last, Now);
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

	FFrameEncoder::FFrameEncoder() = default;
	FFrameEncoder::~FFrameEncoder() = default;

	bool FFrameEncoder::Initialize(const FO3DTransportAudioConfig& Config, const FString& InDefaultStreamLabel, const FString& InDefaultSubject)
	{
		AudioConfig = Config;
		DefaultStreamLabel = InDefaultStreamLabel;
		DefaultSubject = InDefaultSubject;

		if (DefaultStreamLabel.IsEmpty())
		{
			DefaultStreamLabel = DefaultSubject;
		}

		RequestedCodec = SelectCodec(AudioConfig);
#if !O3D_WITH_OPUS
		// SHR-1: Opus is compiled out, so every frame is PCM16 and labelled PCM16.
		if (RequestedCodec == O3DS::EUnifiedCodec::Opus)
		{
			static std::atomic<bool> bWarned{false};
			if (!bWarned.exchange(true))
			{
				UE_LOG(LogO3DAudioCodec, Warning, TEXT("Opus audio was requested, but this build has no Opus support; sending PCM16."));
			}
			RequestedCodec = O3DS::EUnifiedCodec::PCM16;
		}
#endif

		bInitialized = true;
		bOpusReady = false;
		ConsecutiveOpusFailures = 0;
		OpusInitRetryCountdown = 0;
		OpusEncoder.Reset();
		Pending.Reset();
		PendingFrames = 0;
		PendingChannels = 0;
		PendingSampleRate = 0;
		PendingStartTimestampSec = 0.0;
		Stats = FStats();
		return true;
	}

	bool FFrameEncoder::EnsureOpusEncoder(int32 SampleRate, int32 NumChannels)
	{
#if !O3D_WITH_OPUS
		return false;
#else
		if (RequestedCodec != O3DS::EUnifiedCodec::Opus)
		{
			return false;
		}

		const bool bFormatMatches = bOpusReady
			&& OpusEncoder.GetSettings().SampleRate == SampleRate
			&& OpusEncoder.GetSettings().NumChannels == NumChannels;
		if (bFormatMatches)
		{
			return true;
		}

		// A failed initialisation is retried after a while, not on every buffer.
		if (OpusInitRetryCountdown > 0)
		{
			--OpusInitRetryCountdown;
			return false;
		}

		FString Error;
		FO3DAudioOpusEncoder::FSettings Settings;
		Settings.SampleRate = SampleRate;
		Settings.NumChannels = NumChannels;
		Settings.BitrateKbps = AudioConfig.BitrateKbps;
		Settings.FrameSizeMs = OpusFrameSizeMs;
		Settings.bUseVariableBitrate = true;

		if (!OpusEncoder.Initialize(Settings, Error))
		{
			++Stats.OpusInitFailures;
			bOpusReady = false;
			OpusInitRetryCountdown = OpusInitRetryInterval;
			if (ShouldLogOpusWarning())
			{
				UE_LOG(LogO3DAudioCodec, Warning, TEXT("Opus encoder initialisation failed (%s); sending PCM16 and retrying later (%llu failures so far)."),
					*Error, Stats.OpusInitFailures);
			}
			return false;
		}

		bOpusReady = true;
		ConsecutiveOpusFailures = 0;
		Pending.SetNumUninitialized(OpusEncoder.GetFrameSizeSamples() * NumChannels, EAllowShrinking::No);
		PendingFrames = 0;
		PendingChannels = NumChannels;
		PendingSampleRate = SampleRate;
		return true;
#endif
	}

	const FString& FFrameEncoder::ResolveStreamLabel(const FString& Override) const
	{
		return Override.IsEmpty() ? DefaultStreamLabel : Override;
	}

	const FString& FFrameEncoder::ResolveSubject(const FString& Override) const
	{
		return Override.IsEmpty() ? DefaultSubject : Override;
	}

	void FFrameEncoder::EmitPcm16(const FString& Label, const FString& Subject, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec, TArray<FEncodedFrame>& OutFrames)
	{
		const int32 NumSamples = NumFrames * NumChannels;
		FEncodedFrame& Frame = OutFrames.AddDefaulted_GetRef();
		Frame.Codec = O3DS::EUnifiedCodec::PCM16;
		Frame.Meta.StreamLabel = Label;
		Frame.Meta.SubjectName = Subject;
		Frame.Meta.SampleRate = SampleRate;
		Frame.Meta.NumChannels = NumChannels;
		Frame.Meta.TimestampSec = TimestampSec;

		// SHR-18: convert straight into the frame's payload, no intermediate scratch.
		Frame.Encoded.SetNumUninitialized(NumSamples * static_cast<int32>(sizeof(int16)));
		ConvertFloatToPcm16(Interleaved, NumSamples, reinterpret_cast<int16*>(Frame.Encoded.GetData()));
		++Stats.Pcm16Frames;
	}

	void FFrameEncoder::FlushPendingAsPcm16(const FString& Label, const FString& Subject, TArray<FEncodedFrame>& OutFrames)
	{
		if (PendingFrames > 0 && PendingChannels > 0 && PendingSampleRate > 0)
		{
			EmitPcm16(Label, Subject, Pending.GetData(), PendingFrames, PendingChannels, PendingSampleRate, PendingStartTimestampSec, OutFrames);
		}
		PendingFrames = 0;
	}

	void FFrameEncoder::EmitOpusPacket(const FString& Label, const FString& Subject, TArray<FEncodedFrame>& OutFrames)
	{
		int32 FramesEncoded = 0;
		if (OpusEncoder.Encode(Pending.GetData(), PendingFrames, OpusPacketScratch, FramesEncoded) && OpusPacketScratch.Num() > 0)
		{
			FEncodedFrame& Frame = OutFrames.AddDefaulted_GetRef();
			Frame.Meta.StreamLabel = Label;
			Frame.Meta.SubjectName = Subject;
			Frame.Meta.SampleRate = PendingSampleRate;
			Frame.Meta.NumChannels = PendingChannels;
			Frame.Meta.TimestampSec = PendingStartTimestampSec;
			// One exact-size allocation for the payload that leaves with the frame.
			Frame.Encoded.Append(OpusPacketScratch.GetData(), OpusPacketScratch.Num());
			Frame.Codec = O3DS::EUnifiedCodec::Opus;
			PendingFrames = 0;
			ConsecutiveOpusFailures = 0;
			++Stats.OpusPackets;
			return;
		}

		// SHR-2: send this packet's samples as PCM16 (labelled PCM16), count the failure, and
		// recreate the encoder after repeated failures. Opus stays requested.
		++Stats.OpusEncodeFailures;
		if (ShouldLogOpusWarning())
		{
			UE_LOG(LogO3DAudioCodec, Warning, TEXT("Opus encode failed for stream '%s'; sent this packet as PCM16 (%llu failures so far)."),
				*Label, Stats.OpusEncodeFailures);
		}
		FlushPendingAsPcm16(Label, Subject, OutFrames);
		if (++ConsecutiveOpusFailures >= OpusFailuresBeforeReinit)
		{
			OpusEncoder.Reset();
			bOpusReady = false;
			ConsecutiveOpusFailures = 0;
		}
	}

	bool FFrameEncoder::EncodeBuffer(const FString& StreamLabelOverride,
		const FString& SubjectOverride,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec,
		TArray<FEncodedFrame>& OutFrames)
	{
		if (!bInitialized || !Interleaved || NumFrames <= 0)
		{
			return false;
		}

		const int32 EffectiveChannels = NumChannels > 0 ? NumChannels : FMath::Max(AudioConfig.NumChannels, 1);
		const int32 EffectiveSampleRate = SampleRate > 0 ? SampleRate : FMath::Max(AudioConfig.SampleRate, 1);
		if (static_cast<int64>(EffectiveChannels) * NumFrames > MAX_int32 / static_cast<int32>(sizeof(int16)))
		{
			return false;
		}

		const FString& Label = ResolveStreamLabel(StreamLabelOverride);
		const FString& Subject = ResolveSubject(SubjectOverride);

		// A format change ends the current packet: its samples go out as PCM16 rather than
		// being mixed with samples of another format.
		if (PendingFrames > 0 && (EffectiveChannels != PendingChannels || EffectiveSampleRate != PendingSampleRate))
		{
			FlushPendingAsPcm16(Label, Subject, OutFrames);
		}

		if (RequestedCodec == O3DS::EUnifiedCodec::Opus && EnsureOpusEncoder(EffectiveSampleRate, EffectiveChannels))
		{
			const int32 PacketFrames = OpusEncoder.GetFrameSizeSamples();
			int32 Consumed = 0;
			while (Consumed < NumFrames && bOpusReady)
			{
				if (PendingFrames == 0)
				{
					PendingStartTimestampSec = TimestampSec + static_cast<double>(Consumed) / static_cast<double>(EffectiveSampleRate);
				}
				const int32 Take = FMath::Min(PacketFrames - PendingFrames, NumFrames - Consumed);
				FMemory::Memcpy(Pending.GetData() + PendingFrames * EffectiveChannels,
					Interleaved + Consumed * EffectiveChannels,
					static_cast<SIZE_T>(Take) * static_cast<SIZE_T>(EffectiveChannels) * sizeof(float));
				PendingFrames += Take;
				Consumed += Take;
				if (PendingFrames == PacketFrames)
				{
					EmitOpusPacket(Label, Subject, OutFrames);
				}
			}

			if (Consumed < NumFrames)
			{
				// The encoder was dropped after repeated failures; the rest goes out as PCM16.
				EmitPcm16(Label, Subject, Interleaved + Consumed * EffectiveChannels, NumFrames - Consumed, EffectiveChannels, EffectiveSampleRate,
					TimestampSec + static_cast<double>(Consumed) / static_cast<double>(EffectiveSampleRate), OutFrames);
			}
			return true;
		}

		// PCM16 path: PCM16 requested, or Opus unavailable for this format right now. Samples
		// still waiting for an Opus packet go first so nothing is lost or reordered.
		FlushPendingAsPcm16(Label, Subject, OutFrames);
		EmitPcm16(Label, Subject, Interleaved, NumFrames, EffectiveChannels, EffectiveSampleRate, TimestampSec, OutFrames);
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
			if (ShouldLogOpusWarning())
			{
				UE_LOG(LogO3DAudioCodec, Warning, TEXT("Opus decoder initialisation failed (%s)."), *Error);
			}
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
			OutPcm16.SetNumUninitialized(NumSamples, EAllowShrinking::No);
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

	FMultiStreamFrameDecoder::FMultiStreamFrameDecoder(int32 InMaxStreams)
		: MaxStreams(FMath::Max(1, InMaxStreams))
	{
	}

	FMultiStreamFrameDecoder::~FMultiStreamFrameDecoder() = default;

	bool FMultiStreamFrameDecoder::Decode(O3DS::EUnifiedCodec Codec,
		const O3DS::FAudioFrameMeta& Meta,
		const uint8* Payload,
		int32 PayloadSize,
		TArray<int16>& OutPcm16)
	{
		const FString Key = Meta.SourceGuid.ToString(EGuidFormats::Digits) + TEXT("|") + Meta.StreamLabel;

		TUniquePtr<FStream>* Found = Streams.Find(Key);
		if (!Found)
		{
			while (Streams.Num() >= MaxStreams)
			{
				FString OldestKey;
				uint64 OldestUse = MAX_uint64;
				for (const TPair<FString, TUniquePtr<FStream>>& Pair : Streams)
				{
					if (Pair.Value->LastUse < OldestUse)
					{
						OldestUse = Pair.Value->LastUse;
						OldestKey = Pair.Key;
					}
				}
				Streams.Remove(OldestKey);
			}
			Found = &Streams.Add(Key, MakeUnique<FStream>());
		}

		FStream& Stream = **Found;
		Stream.LastUse = ++UseCounter;
		return Stream.Decoder.Decode(Codec, Meta, Payload, PayloadSize, OutPcm16);
	}

	void ConvertFloatToPcm16(const float* In, int32 NumSamples, int16* Out)
	{
		if (!In || !Out)
		{
			return;
		}
		for (int32 Index = 0; Index < NumSamples; ++Index)
		{
			Out[Index] = FloatToPcm16(In[Index]);
		}
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
		// SHR-18: serialize the audio payload after a reserved envelope header, then fill the
		// header in place, instead of building the payload and copying it into a second buffer.
		if (Frame.Encoded.Num() <= 0
			|| !SerializeEncodedAudioFrameAfterPrefix(Frame.Codec, Frame.Meta, Frame.Encoded.GetData(), Frame.Encoded.Num(), O3DS::UnifiedWireHeaderSize, OutMessage))
		{
			return false;
		}

		return O3DS::WriteUnifiedHeaderInPlace(O3DS::EUnifiedKind::Audio, Frame.Codec, TimestampSec, OutMessage);
	}
}
