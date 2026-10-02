// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "O3DAudioFrameCodec.h"
#include "O3DUnifiedMessage.h"

/*
 * The shared receive demux (ADR 0007 item 7, WP-A1 step 4; TRB-37, TRB-38, SHR-15, ADR 0011).
 *
 * Every receiver hands what arrived to one of these. It classifies a buffer once, by the unified
 * envelope header (ADR 0009), and routes it:
 * - mocap to the frame consumer (ISerializedFrameConsumer::SubmitFrame);
 * - audio to the audio sink, decoding Opus with one decoder per (SourceGuid, StreamLabel)
 *   (O3DAudio::FMultiStreamFrameDecoder, LRU-capped; SHR-15);
 * - control to the control sink, only when O3DS::TryGetControlPayload accepts it (ADR 0011: a
 *   malformed control envelope is dropped, never treated as mocap);
 * - an envelope with no payload is a keepalive (TCP, TRB-6); unknown kinds are ignored.
 * Every result is counted, so a receiver can report rejects without logging each packet from an
 * untrusted peer (SHR-8).
 *
 * The demux holds the consumer and both sinks strongly while the receiver runs and releases them
 * in ReleaseSinks(), which the receiver calls from Stop() (ADR 0007 item 3, TRF-38).
 *
 * Threading: not thread-safe. One thread uses it: the thread that calls the receiver's Poll()
 * (the game thread), which is also where the setters are called (before Start, or between Poll
 * calls). The sinks themselves may be called from it only.
 */

/** What the demux did with one buffer. */
enum class EO3DDemuxResult : uint8
{
	/** Mocap: delivered to the consumer, or counted as undelivered when there is none. */
	Mocap,
	/** Audio: delivered to the audio sink, or skipped without parsing when there is none. */
	Audio,
	/** A well-formed control envelope: delivered to the control sink, or skipped without one. */
	Control,
	/** An envelope with an empty payload (a sender keepalive). */
	Keepalive,
	/** Audio that failed to parse (bad header or metadata, SHR-8) or to decode. Dropped. */
	AudioRejected,
	/** Empty input, a truncated envelope, or a malformed control envelope. Dropped. */
	Malformed,
	/** Larger than FO3DReceiveDemuxSettings::MaxMessageBytes. Dropped. */
	Oversize,
	/** An envelope kind this build does not know (a newer sender). Ignored. */
	UnknownKind,
};

OPEN3DSHARED_API const TCHAR* LexToString(EO3DDemuxResult Result);

/** Settings of FO3DUnifiedReceiveDemux. */
struct FO3DReceiveDemuxSettings
{
	/** The receiving stream; passed to the control sink, and the subject of mocap without one. */
	FString StreamId;

	/**
	 * Accept a buffer without an envelope as raw mocap (senders before the unified envelope; the
	 * TCP, UDP and NNG receivers have always done so). When false it is Malformed.
	 */
	bool bAcceptRawMocap = true;

	/** Largest buffer ProcessMessage accepts. 0 means no limit beyond the envelope's own. */
	int32 MaxMessageBytes = O3DS::UnifiedWireHeaderSize + O3DS::UnifiedMaxPayloadSize;

	/** Audio streams decoded at once; the least recently used decoder is evicted (SHR-15). */
	int32 MaxAudioStreams = 16;
};

/** Counters, one per outcome. */
struct FO3DReceiveDemuxStats
{
	int64 Mocap = 0;
	int64 MocapBytes = 0;
	int64 MocapWithoutConsumer = 0;
	int64 Audio = 0;
	int64 AudioWithoutSink = 0;
	int64 AudioRejected = 0;
	int64 Control = 0;
	int64 ControlWithoutSink = 0;
	int64 Keepalive = 0;
	int64 Malformed = 0;
	int64 Oversize = 0;
	int64 UnknownKind = 0;

	/** Everything dropped as unusable: AudioRejected + Malformed + Oversize. */
	int64 GetRejected() const { return AudioRejected + Malformed + Oversize; }
};

class OPEN3DSHARED_API FO3DUnifiedReceiveDemux
{
public:
	explicit FO3DUnifiedReceiveDemux(const FO3DReceiveDemuxSettings& InSettings = FO3DReceiveDemuxSettings());
	~FO3DUnifiedReceiveDemux();

	FO3DUnifiedReceiveDemux(const FO3DUnifiedReceiveDemux&) = delete;
	FO3DUnifiedReceiveDemux& operator=(const FO3DUnifiedReceiveDemux&) = delete;

	/** Replaces the settings; resets the audio decoders when MaxAudioStreams changes. */
	void SetSettings(const FO3DReceiveDemuxSettings& InSettings);
	const FO3DReceiveDemuxSettings& GetSettings() const { return Settings; }

	void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) { Consumer = InConsumer; }
	void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& InSink) { AudioSink = InSink; }
	void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& InSink) { ControlSink = InSink; }

	bool HasConsumer() const { return Consumer.IsValid(); }
	bool HasAudioSink() const { return AudioSink.IsValid(); }
	bool HasControlSink() const { return ControlSink.IsValid(); }

	/** Drops the consumer and both sinks and every audio decoder. Call from the receiver's Stop(). */
	void ReleaseSinks();

	/**
	 * Classifies one received buffer (an envelope, or raw mocap) and routes it; the one entry
	 * point for in-band transports (TCP, UDP, NNG; Loopback for audio and control). Subject is
	 * the mocap subject; empty means the stream id. Data is not kept after the call.
	 */
	EO3DDemuxResult ProcessMessage(const uint8* Data, int32 Size, double ReceiveTimeSec, const FString& Subject = FString());

	/** Bytes already known to be one serialized mocap frame, delivered without a copy. */
	EO3DDemuxResult DeliverMocap(const FString& Subject, const TArray<uint8>& Frame, double ReceiveTimeSec);

	/** A serialized audio payload (no envelope) whose codec the caller knows, e.g. from the envelope or track. */
	EO3DDemuxResult DeliverAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 Size);

	/** A serialized audio payload (no envelope); the codec is read from its header (MoQ's audio track). */
	EO3DDemuxResult DeliverAudioPayload(const uint8* Payload, int32 Size);

	/** An already parsed audio frame (metadata and encoded bytes), e.g. from an FFI audio callback. */
	EO3DDemuxResult DeliverAudioFrame(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* Encoded, int32 Size);

	/** A buffer that should be a control envelope (a control track or label). Malformed when it is not. */
	EO3DDemuxResult DeliverControlEnvelope(const uint8* Data, int32 Size, double ReceiveTimeSec);

	const FO3DReceiveDemuxStats& GetStats() const { return Stats; }
	void ResetStats() { Stats = FO3DReceiveDemuxStats(); }

	/** Audio streams with a live decoder. For tests. */
	int32 GetNumAudioDecoders() const { return AudioDecoder->GetNumStreams(); }

private:
	FO3DReceiveDemuxSettings Settings;
	TSharedPtr<ISerializedFrameConsumer> Consumer;
	TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
	TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ControlSink;
	/** Rebuilt when MaxAudioStreams changes (the decoder map takes its cap at construction). */
	TUniquePtr<O3DAudio::FMultiStreamFrameDecoder> AudioDecoder;
	/** Reused for enveloped and raw mocap until ISerializedFrameConsumer takes a view (ADR 0007 step 5). */
	TArray<uint8> MocapScratch;
	TArray<int16> DecodedPcm;
	FO3DReceiveDemuxStats Stats;
};
