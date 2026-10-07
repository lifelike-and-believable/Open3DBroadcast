// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Templates/UniquePtr.h"

#include "Transport/O3DTransportTypes.h"
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
		/** PCM16 samples are little-endian (ADR 0009 item 4). */
		TArray<uint8> Encoded;
		/** Counts this stream's frames (FFrameEncoder), wraps; the envelope v2 sequence number. */
		uint32 Sequence = 0;
	};

	/**
	 * Converts interleaved float PCM from one audio stream into the configured codec (PCM16 or
	 * Opus) and prepares metadata for transport. Stateful: holds one Opus encoder and the
	 * samples waiting to complete the next Opus packet, so use one instance per stream. Not
	 * thread-safe.
	 *
	 * Codec labelling (SHR-1): a frame's Codec is set only after that frame was produced by that
	 * codec. When Opus is compiled out, cannot run at the stream's format, or fails, frames are
	 * PCM16 and labelled PCM16.
	 *
	 * Opus framing (SHR-2): capture buffers of any size are accumulated and emitted as packets of
	 * exactly one Opus frame (SampleRate * FrameSizeMs / 1000 frames), so one call returns zero or
	 * more frames. An Opus failure is counted and logged at Warning; it never disables Opus for
	 * the rest of the stream (the encoder is recreated after repeated failures, and a failed
	 * initialisation is retried).
	 */
	class OPEN3DSHARED_API FFrameEncoder
	{
	public:
		/** Opus frame duration used for packets, in ms. */
		static constexpr int32 OpusFrameSizeMs = 20;

		/** Consecutive packet failures after which the Opus encoder is recreated. */
		static constexpr int32 OpusFailuresBeforeReinit = 3;

		/** Buffers submitted as PCM16 after a failed Opus initialisation before it is retried. */
		static constexpr int32 OpusInitRetryInterval = 250;

		struct FStats
		{
			uint64 OpusPackets = 0;
			uint64 Pcm16Frames = 0;
			uint64 OpusEncodeFailures = 0;
			uint64 OpusInitFailures = 0;
		};

		FFrameEncoder();
		~FFrameEncoder();
		FFrameEncoder(const FFrameEncoder&) = delete;
		FFrameEncoder& operator=(const FFrameEncoder&) = delete;

		bool Initialize(const FO3DTransportAudioConfig& Config, const FString& InDefaultStreamLabel, const FString& InDefaultSubject);

		/**
		 * Encode one capture buffer and append the resulting frames (zero or more) to OutFrames.
		 * TimestampSec is the capture time of the buffer's first frame; each emitted frame carries
		 * the capture time of its own first frame. Returns false when the input is invalid or the
		 * encoder is not initialised; true otherwise, including when the samples were only
		 * buffered towards the next Opus packet.
		 */
		bool EncodeBuffer(const FString& StreamLabelOverride,
			const FString& SubjectOverride,
			const float* Interleaved,
			int32 NumFrames,
			int32 NumChannels,
			int32 SampleRate,
			double TimestampSec,
			TArray<FEncodedFrame>& OutFrames);

		const FO3DTransportAudioConfig& GetConfig() const { return AudioConfig; }

		/** The codec the configuration asks for. */
		O3DS::EUnifiedCodec GetRequestedCodec() const { return RequestedCodec; }

		/** Frames per channel waiting for the next Opus packet. */
		int32 GetPendingFrames() const { return PendingFrames; }

		const FStats& GetStats() const { return Stats; }

	private:
		bool EnsureOpusEncoder(int32 SampleRate, int32 NumChannels);
		void EmitOpusPacket(const FString& Label, const FString& Subject, TArray<FEncodedFrame>& OutFrames);
		void EmitPcm16(const FString& Label, const FString& Subject, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec, TArray<FEncodedFrame>& OutFrames);
		void FlushPendingAsPcm16(const FString& Label, const FString& Subject, TArray<FEncodedFrame>& OutFrames);
		const FString& ResolveStreamLabel(const FString& Override) const;
		const FString& ResolveSubject(const FString& Override) const;

		FO3DTransportAudioConfig AudioConfig;
		FString DefaultStreamLabel;
		FString DefaultSubject;
		O3DS::EUnifiedCodec RequestedCodec = O3DS::EUnifiedCodec::PCM16;

		bool bInitialized = false;
		bool bOpusReady = false;
		int32 ConsecutiveOpusFailures = 0;
		int32 OpusInitRetryCountdown = 0;
		FO3DAudioOpusEncoder OpusEncoder;
		/** Encoded bytes of the last packet; keeps its allocation between packets (SHR-18). */
		TArray<uint8> OpusPacketScratch;

		/** Samples waiting for the next Opus packet, and their format and capture time. */
		TArray<float> Pending;
		int32 PendingFrames = 0;
		int32 PendingChannels = 0;
		int32 PendingSampleRate = 0;
		double PendingStartTimestampSec = 0.0;

		/** Sequence number of the next frame emitted (FEncodedFrame::Sequence). */
		uint32 NextSequence = 0;

		FStats Stats;
	};

	/**
	 * Helper that converts encoded payloads back to PCM16 for playback/processing.
	 * Holds one stateful Opus decoder, so it serves a single stream only; receivers use
	 * FMultiStreamFrameDecoder below. Not thread-safe: one thread uses an instance.
	 */
	class OPEN3DSHARED_API FFrameDecoder
	{
	public:
		FFrameDecoder() = default;
		// Non-copyable: it owns a stateful Opus decoder.
		FFrameDecoder(const FFrameDecoder&) = delete;
		FFrameDecoder& operator=(const FFrameDecoder&) = delete;

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

	/**
	 * One FFrameDecoder per audio stream, keyed by (SourceGuid, StreamLabel) (SHR-15).
	 * Opus decoding is stateful, so interleaving two publishers or two labels through one
	 * decoder corrupts both. Same Decode() signature as FFrameDecoder. Not thread-safe:
	 * owned and called by one receive thread (the thread that calls Poll()).
	 */
	class OPEN3DSHARED_API FMultiStreamFrameDecoder
	{
	public:
		explicit FMultiStreamFrameDecoder(int32 InMaxStreams = 16);
		~FMultiStreamFrameDecoder();
		// Non-copyable: it owns stateful decoders. The explicit deletes also stop MSVC from
		// instantiating a copy constructor for this exported class, which fails on the
		// TMap of TUniquePtr (C2280).
		FMultiStreamFrameDecoder(const FMultiStreamFrameDecoder&) = delete;
		FMultiStreamFrameDecoder& operator=(const FMultiStreamFrameDecoder&) = delete;

		bool Decode(O3DS::EUnifiedCodec Codec,
			const O3DS::FAudioFrameMeta& Meta,
			const uint8* Payload,
			int32 PayloadSize,
			TArray<int16>& OutPcm16);

		int32 GetNumStreams() const { return Streams.Num(); }
		void Reset() { Streams.Reset(); }

	private:
		struct FStream
		{
			FFrameDecoder Decoder;
			uint64 LastUse = 0;
		};

		int32 MaxStreams = 16;
		uint64 UseCounter = 0;
		TMap<FString, TUniquePtr<FStream>> Streams;
	};

	/** Float [-1, 1] to PCM16 with clamping and round-to-nearest. */
	OPEN3DSHARED_API void ConvertFloatToPcm16(const float* In, int32 NumSamples, int16* Out);

	/** Serialise an encoded frame into the transport-neutral audio payload format. */
	OPEN3DSHARED_API bool SerializeForTransport(const FEncodedFrame& Frame, TArray<uint8>& OutPayload);

	/**
	 * Wrap an encoded frame in the unified message envelope for network transports. The audio
	 * payload is serialized straight after the envelope header in one buffer (SHR-18).
	 */
	OPEN3DSHARED_API bool CreateUnifiedAudioMessage(const FEncodedFrame& Frame, double TimestampSec, TArray<uint8>& OutMessage);

	/**
	 * PCM16 on the wire is little-endian (ADR 0009 item 4): converts NumBytes of host-order
	 * samples in place, and back. A no-op on little-endian hosts, which is every shipped platform.
	 */
	OPEN3DSHARED_API void Pcm16HostToWire(uint8* Bytes, int32 NumBytes);
	OPEN3DSHARED_API void Pcm16WireToHost(uint8* Bytes, int32 NumBytes);
}

