#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

#include "O3DTransportTypes.h"
#include "O3DAudioOpus.h"
#include "O3DAudioSerialization.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DAudioCodec, Log, All);

namespace O3DAudio
{
	/** Normalise a codec string for comparisons (trim + lowercase). */
	OPEN3DSHARED_API FString SanitizeCodecString(const FString& InCodec);

	/** Determine which codec a transport should use based on configuration. */
	OPEN3DSHARED_API O3DS::EUnifiedCodec SelectCodec(const FO3DTransportAudioConfig& Config);

	/** Encoded audio payload paired with metadata. */
	struct OPEN3DSHARED_API FEncodedFrame
	{
		O3DS::EUnifiedCodec Codec = O3DS::EUnifiedCodec::PCM16;
		O3DS::FAudioFrameMeta Meta;
		TArray<uint8> Encoded;
	};

	/**
	 * Helper that converts interleaved float PCM into the configured codec (PCM16 or Opus) and
	 * prepares metadata for transport.
	 */
	class OPEN3DSHARED_API FFrameEncoder
	{
	public:
		bool Initialize(const FO3DTransportAudioConfig& Config, const FString& InDefaultStreamLabel, const FString& InDefaultSubject);

		bool BuildEncodedFrame(const FString& StreamLabelOverride,
			const FString& SubjectOverride,
			const float* Interleaved,
			int32 NumFrames,
			int32 NumChannels,
			int32 SampleRate,
			double TimestampSec,
			FEncodedFrame& OutFrame);

		const FO3DTransportAudioConfig& GetConfig() const { return AudioConfig; }
		O3DS::EUnifiedCodec GetActiveCodec() const { return ActiveCodec; }

	private:
		bool EnsureOpusEncoder(int32 SampleRate, int32 NumChannels);
		FString ResolveStreamLabel(const FString& Override) const;
		FString ResolveSubject(const FString& Override) const;

		FO3DTransportAudioConfig AudioConfig;
		FString DefaultStreamLabel;
		FString DefaultSubject;
		O3DS::EUnifiedCodec ActiveCodec = O3DS::EUnifiedCodec::PCM16;

		bool bInitialized = false;
		bool bOpusReady = false;
		FO3DAudioOpusEncoder OpusEncoder;
		TArray<int16> PCM16Scratch;
		int32 PCM16ScratchCapacity = 0;
	};

	/**
	 * Helper that converts encoded payloads back to PCM16 for playback/processing.
	 */
	class OPEN3DSHARED_API FFrameDecoder
	{
	public:
		bool Decode(O3DS::EUnifiedCodec Codec,
			const O3DS::FAudioFrameMeta& Meta,
			const uint8* Payload,
			int32 PayloadSize,
			TArray<int16>& OutPcm16);

	private:
		bool EnsureOpusDecoder(int32 SampleRate, int32 NumChannels);

		FO3DAudioOpusDecoder OpusDecoder;
		int32 CachedSampleRate = 0;
		int32 CachedNumChannels = 0;
		bool bOpusReady = false;
	};

	/** Serialise an encoded frame into the transport-neutral audio payload format. */
	OPEN3DSHARED_API bool SerializeForTransport(const FEncodedFrame& Frame, TArray<uint8>& OutPayload);

	/** Wrap an encoded frame in the unified message envelope for network transports. */
	OPEN3DSHARED_API bool CreateUnifiedAudioMessage(const FEncodedFrame& Frame, double TimestampSec, TArray<uint8>& OutMessage);
}

