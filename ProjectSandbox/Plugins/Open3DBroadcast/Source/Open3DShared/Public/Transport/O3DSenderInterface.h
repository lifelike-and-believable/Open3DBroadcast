// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DTransportTypes.h"

/*
 * The sender side of the transport interface (ADR 0007 items 1 and 3). Moved here from
 * Open3DSender so transports and the Open3DBroadcastWebRTC add-on can depend on Open3DShared
 * alone (WP-A1 step 6 removed the old Open3DSender forwarding header). The control channel
 * (ADR 0011) appended SupportsControl and SendControl (version 2). WP-A1 PR 3 (version 4) replaced
 * the bool results with FO3DTransportResult and EO3DSendResult, made SendSerialized pure virtual
 * with an owned FO3DSendPayload, and added capabilities and connection state; SupportsAudio and
 * SupportsControl became non-virtual forwarders to GetCapabilities(), and WP-A1 step 6 removed
 * them: use GetCapabilities().bAudioSend and GetCapabilities().bControl.
 *
 * Threading contract (ADR 0007 item 3), written next to each method below. "Game thread" means
 * the thread that owns the instance; transports must not block it.
 */

/** Interface for audio sinks provided by transports that support PCM ingestion. */
class OPEN3DSHARED_API IO3DSenderAudioSink
{
public:
    virtual ~IO3DSenderAudioSink() = default;

    /** Submit interleaved floating point PCM samples. Returns false if the frame was dropped. */
    virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) = 0;

    /** Notification that the capture path has stopped producing frames. */
    virtual void OnCaptureStopped() {}
};

/** Interface implemented by all transport sender instances. */
class OPEN3DSHARED_API IOpen3DSender
{
public:
    virtual ~IOpen3DSender() = default;

    /**
     * Validates Config and allocates what Start needs. Game thread; non-blocking. Leaves the
     * connection state as it is (Idle for a new instance). InvalidConfig when Config is unusable,
     * ResourceUnavailable when a library or subsystem is missing. No SendSerialized, SendControl
     * or audio sink call may be in flight: Initialize reconfigures the instance. Metrics go to
     * FO3DRuntimeContext::OrDefault(Config.Context) from here on (ADR 0012 item 3).
     */
    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) = 0;

    /**
     * Begins networking. Game thread; non-blocking: a connection is made on the transport's own
     * thread and reported through the connection state. NotRunning without a successful
     * Initialize (the state is left as it is); AddressInUse, ConnectFailed or ResourceUnavailable
     * when it cannot start, after moving the state to Failed. On success the state is Connecting
     * or Connected.
     */
    virtual FO3DTransportResult Start() = 0;

    /**
     * Stops networking and releases resources. Game thread; idempotent; non-blocking (it joins
     * the transport's own threads, which never wait on the game thread). Safe while SendSerialized,
     * SendControl or an audio sink call is in flight; later sends return NotRunning. Moves the
     * state to Idle; no state callback runs after Stop returns.
     */
    virtual void Stop() = 0;

    /**
     * Sends one already-serialized frame. Any thread (the pose pipeline's worker once ADR 0008
     * lands; the game thread today); thread-safe; never blocks. Takes ownership of the payload.
     * The frame pipeline produces the final wire bytes (full sync or delta/residual update), so
     * the transport transmits them as they are.
     *
     * Returns Queued when accepted, NotRunning before Start or after Stop, NotConnected while
     * there is no peer or session, Invalid for an empty payload, TooLarge above
     * FO3DTransportCapabilities::MaxPayloadBytes, and DroppedBackpressure when the transport's
     * queue is full (the frame is counted in FO3DTransportStats::DroppedFrames). A refused frame is
     * not retried by the caller.
     */
    virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) = 0;

    /** Upkeep. Game thread; never blocks. */
    virtual void Tick(float DeltaSeconds) = 0;

    /** Snapshot of the counters and connection state. Any thread; inexpensive. */
    virtual FO3DTransportStats GetStats() const = 0;

    /**
     * What this sender can do with the config it was initialized with (before Initialize: with
     * an empty config). The same values as FO3DTransportDescriptor::GetCapabilities for that
     * config. Any thread.
     */
    virtual FO3DTransportCapabilities GetCapabilities() const = 0;

    /** Current connection state. Any thread; lock-free. */
    virtual EO3DConnectionState GetConnectionState() const = 0;

    /**
     * Sets the callback run on every connection-state change; null removes it. Game thread,
     * before Start. The callback may run on any thread and must not call back into the
     * transport (see FO3DConnectionStateCallback).
     */
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) = 0;

    /** Audio sink factory, when GetCapabilities().bAudioSend. Game thread. Default returns nullptr. */
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) { return nullptr; }

    /**
     * ADR 0005 (vi): Callback is called when the sender gains a peer that has seen nothing yet (a
     * receiver connects, a subscriber or pipe is added), so the next frame can be a full sync
     * instead of waiting for the periodic one. It may run on any thread, during or after
     * SendSerialized, and must only record the fact (the sender pipeline sets an atomic flag). A
     * transport that calls it reports bPeerJoinSignal. An empty callback clears it. Default: not
     * supported, nothing is stored.
     */
    virtual void SetPeerJoinedCallback(FO3DPeerJoinedCallback Callback) { (void)Callback; }

    /**
     * WP-R1 (mid-project review TR-1): Callback is called when the sender drops a mocap frame
     * after SendSerialized returned Queued (an age limit, an eviction, a full send buffer). The
     * frame may have been a full sync or a residual update receivers needed, so the next frame
     * should be a full sync. Same threading rules as SetPeerJoinedCallback. An empty callback
     * clears it. Default: not supported, nothing is stored.
     */
    virtual void SetFramesDroppedCallback(FO3DFramesDroppedCallback Callback) { (void)Callback; }

    /**
     * Sends one control envelope (O3DS::WriteControlEnvelope output) to every receiver of this
     * stream (docs/adr/0011-control-channel.md, item 6). Game thread in v1; implementations are
     * thread-safe, never block, and copy the bytes. Not routed through SendSerialized or the pose
     * pipeline, so control is never dropped as an old pose or counted as a mocap frame.
     *
     * Returns Queued when accepted, NotRunning before Start or after Stop, NotConnected while
     * there is no peer or session, Invalid when the bytes are not a control envelope,
     * DroppedBackpressure when the queue is full, and Unsupported when the transport (or its
     * current role) carries no control. The caller (FO3DControlPublisher) retries what was refused.
     * Default returns Unsupported.
     */
    virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len)
    {
        (void)Envelope; (void)Len;
        return EO3DSendResult::Unsupported;
    }
};

/** Creates one sender instance. Called on the game thread, outside any registry lock. */
using FO3DSenderFactory = TFunction<TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>()>;
