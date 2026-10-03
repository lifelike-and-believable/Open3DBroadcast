// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DAudioFrameCodec.h"
#include "O3DLifetimeGate.h"
#include "O3DSinkAudioEncoder.h"

#include <atomic>

/*
 * Sender audio sinks (ADR 0007 item 7 and its WP-S5 addendum). Moved here from Open3DSender in
 * WP-A1 step 4, so transports depend on Open3DShared alone.
 *
 * - FO3DSenderAudioSinkBase validates a submission and normalises its stream label.
 * - FO3DQueuedSenderAudioSink is the shared sink a migrated transport hands out: it holds the
 *   transport's FO3DAudioPublishState, never the sender, encodes on the calling (audio) thread
 *   with its own encoders, and enqueues the bytes on the transport's FO3DSendQueue as audio items.
 * - FO3DGatedSenderAudioSink is the WP-S5 per-transport form, kept for the transports that have
 *   not migrated to the shared queue yet (TCP, UDP, NNG, MoQ); it goes when the last one has.
 */

/**
 * Validates audio submissions and normalises stream labels before forwarding PCM frames to the
 * concrete sink.
 */
class OPEN3DSHARED_API FO3DSenderAudioSinkBase : public IO3DSenderAudioSink
{
public:
	explicit FO3DSenderAudioSinkBase(FO3DTransportAudioConfig InConfig)
		: AudioConfig(MoveTemp(InConfig))
	{
	}

	virtual ~FO3DSenderAudioSinkBase() = default;

	virtual bool SubmitPcm(const FString& StreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override final;

protected:
	virtual bool OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) = 0;

	const FO3DTransportAudioConfig& GetAudioConfig() const { return AudioConfig; }

private:
	FO3DTransportAudioConfig AudioConfig;
};

/**
 * Sender audio sink that never references its sender (WP-S5 form, see the file comment).
 *
 * It holds a strong reference to the sender's lifetime gate, the gate epoch that was current
 * when the sink was created, and its own per-label encoders built from an immutable config
 * snapshot. Every submit enters the gate first; after the sender's Stop() has closed the gate,
 * or once a newer session has started, submits return false without touching anything.
 *
 * Subclasses implement OnSubmitGated(), which runs inside the gate and must only use the
 * sink's own members and the transport's shared publish state (never the sender).
 */
class OPEN3DSHARED_API FO3DGatedSenderAudioSink : public FO3DSenderAudioSinkBase
{
public:
	FO3DGatedSenderAudioSink(FO3DTransportAudioConfig InConfig,
		TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> InGate,
		FO3DSinkAudioEncoder::FSettings InEncoderSettings);

	/** Epoch this sink is bound to (0 if the gate was closed when it was created). */
	uint64 GetBoundEpoch() const { return BoundEpoch; }

protected:
	virtual bool OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override final;

	virtual bool OnSubmitGated(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) = 0;

	FO3DSinkAudioEncoder& GetEncoder() { return Encoder; }

private:
	TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate;
	const uint64 BoundEpoch;
	FO3DSinkAudioEncoder Encoder;
};

/** How FO3DQueuedSenderAudioSink turns an encoded frame into the bytes it enqueues. */
enum class EO3DAudioWireFormat : uint8
{
	/** A unified envelope of kind Audio (O3DAudio::CreateUnifiedAudioMessage): in-band transports (Loopback, TCP, UDP, NNG). */
	UnifiedEnvelope,
	/** The bare audio payload (O3DAudio::SerializeForTransport): transports with a separate audio channel (MoQ). */
	AudioPayload,
};

/**
 * What a transport's audio path shares with its sinks (ADR 0007 item 7, the WP-S5 publish state).
 *
 * Holds the lifetime gate, a reference to the transport's send queue, the last-subject slot and
 * an audio byte counter; never a pointer to the sender, a receiver, a UObject, a socket or an FFI
 * handle. Its destructor frees memory only, so any thread may drop the last reference (a sink kept
 * alive by the audio capture after its sender is gone).
 *
 * The sender creates one per session (Initialize), opens it in Initialize/Start and closes it in
 * Stop() before it releases anything the queue's consumer uses. Close() returns only after every
 * submit already inside the gate has left, and later submits fail fast, so a sink can never
 * enqueue after Stop() returned, nor into a later session (the gate epoch).
 *
 * Peer gate: a transport that has nowhere to send audio yet (a TCP sender with no receiver
 * connected) clears SetPeerReady, and sinks then refuse PCM without encoding it, as the WP-S5
 * per-transport sinks did. It starts true, so transports without the notion never touch it.
 *
 * Threading: Open, Close: the game thread (the sender's owner). Everything else: any thread.
 */
class OPEN3DSHARED_API FO3DAudioPublishState
{
public:
	explicit FO3DAudioPublishState(TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> InQueue,
		EO3DAudioWireFormat InWireFormat = EO3DAudioWireFormat::UnifiedEnvelope);

	FO3DAudioPublishState(const FO3DAudioPublishState&) = delete;
	FO3DAudioPublishState& operator=(const FO3DAudioPublishState&) = delete;

	/** Opens a new gate epoch if closed; returns the current epoch. */
	uint64 Open() { return Gate.Open(); }

	/** Closes the gate and waits for in-flight submits. Idempotent. */
	void Close() { Gate.Close(); }

	bool IsOpen() const { return Gate.IsOpen(); }
	uint64 GetEpoch() const { return Gate.GetEpoch(); }
	FO3DLifetimeGate& GetGate() { return Gate; }

	FO3DSendQueue& GetQueue() const { return *Queue; }
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe>& GetQueueRef() const { return Queue; }

	/** The subject the sender last sent a frame for; audio frames carry it in their metadata. */
	FO3DAudioSubjectSlot& GetSubjectSlot() { return SubjectSlot; }

	EO3DAudioWireFormat GetWireFormat() const { return WireFormat; }

	/** False while the transport has no peer to send audio to; sinks refuse PCM then. Any thread. */
	void SetPeerReady(bool bReady) { bPeerReady.store(bReady, std::memory_order_release); }
	bool IsPeerReady() const { return bPeerReady.load(std::memory_order_acquire); }

	/** Audio bytes the sinks enqueued since the state was created. */
	int64 GetAudioBytesQueued() const { return AudioBytesQueued.load(std::memory_order_relaxed); }
	void AddAudioBytesQueued(int64 Bytes) { AudioBytesQueued.fetch_add(Bytes, std::memory_order_relaxed); }

private:
	FO3DLifetimeGate Gate;
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue;
	FO3DAudioSubjectSlot SubjectSlot;
	const EO3DAudioWireFormat WireFormat;
	std::atomic<int64> AudioBytesQueued{ 0 };
	std::atomic<bool> bPeerReady{ true };
};

using FO3DAudioPublishStateRef = TSharedRef<FO3DAudioPublishState, ESPMode::ThreadSafe>;

/**
 * The shared sender audio sink (ADR 0007 item 7; TRB-10, TRB-11, TRB-30, TRB-35, TRF-1).
 *
 * SubmitPcm, on any thread: enter the publish state's gate in the epoch the sink was created in
 * (fails fast once the sender stopped or restarted), refuse when the state has no peer
 * (IsPeerReady), encode with the sink's own per-label
 * encoders (FO3DSinkAudioEncoder; Opus may return zero or several frames per buffer), wrap each
 * frame (MakeAudioBytes) and enqueue it as an audio item. Returns false when the gate is closed,
 * encoding rejected the input, or the queue refused a frame (audio full: DroppedBackpressure).
 * No lock is ever held across a socket or FFI call: the transport's worker sends the bytes.
 */
class OPEN3DSHARED_API FO3DQueuedSenderAudioSink : public FO3DSenderAudioSinkBase
{
public:
	FO3DQueuedSenderAudioSink(FO3DAudioPublishStateRef InState,
		FO3DTransportAudioConfig InConfig,
		FO3DSinkAudioEncoder::FSettings InEncoderSettings);

	FO3DQueuedSenderAudioSink(const FO3DQueuedSenderAudioSink&) = delete;
	FO3DQueuedSenderAudioSink& operator=(const FO3DQueuedSenderAudioSink&) = delete;

	/** Epoch this sink is bound to (0 if the state was closed when it was created: the sink never accepts). */
	uint64 GetBoundEpoch() const { return BoundEpoch; }

protected:
	virtual bool OnSubmitPcmInternal(const FString& ResolvedStreamLabel,
		const float* Interleaved,
		int32 NumFrames,
		int32 NumChannels,
		int32 SampleRate,
		double TimestampSec) override;

	/** The bytes to enqueue for one encoded frame. Default: per the state's EO3DAudioWireFormat. Inside the gate. */
	virtual bool MakeAudioBytes(const O3DAudio::FEncodedFrame& Frame, TArray<uint8>& OutBytes) const;

	FO3DSinkAudioEncoder& GetEncoder() { return Encoder; }
	FO3DAudioPublishState& GetState() const { return *State; }

private:
	const FO3DAudioPublishStateRef State;
	const uint64 BoundEpoch;
	FO3DSinkAudioEncoder Encoder;
};
