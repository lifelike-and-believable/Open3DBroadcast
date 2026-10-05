// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DSenderAudioSinkBase.h"
#include "Transport/O3DTransportWorker.h"
#include "Shared/NngHelpers.h"
#include "O3DPerformanceMetrics.h"
#include "O3DRuntimeContext.h"

#include <atomic>

/**
 * Context for the NNG pipe-notify callback, reached through an opaque token (TRF-12 pattern).
 * Holds atomics only, so an NNG thread may drop the last reference.
 */
struct FNngSenderPipeContext
{
    std::atomic<int32> PipeCount{0};
    std::atomic<bool> bConnected{false};
    /** True for listening/pub sockets, which stay "connected" with no pipes. */
    std::atomic<bool> bConnectedWithoutPipes{true};

    /** IOpen3DSender::SetPeerJoinedCallback's callback, called for each added pipe (ADR 0005 (vi)). */
    FCriticalSection PeerJoinedLock;
    FO3DPeerJoinedCallback PeerJoined;
};

/**
 * NNG sender on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4d).
 *
 * SendSerialized, SendControl and the audio sinks (FO3DQueuedSenderAudioSink) only enqueue on one
 * FO3DSendQueue; an FO3DTransportWorker owns the socket and hands each item to nng_send with
 * NNG_FLAG_NONBLOCK, so no caller ever blocks on NNG (TRB-33, TRB-34).
 *
 * Queue policy: EO3DMocapOverflow::RefuseNewest with nng.qmax as the byte limit of frames and,
 * separately, of audio; control has the queue's own cap. That is what the sender always did
 * (a full queue refused the newest payload), and it is right for every mode: the worker never
 * holds a backlog, because a frame NNG cannot take at once (no peer, or NNG's own send buffer
 * full) is dropped at the worker, oldest first. So a slow peer already costs the oldest frames,
 * and the application queue only fills when the worker itself falls behind.
 *
 * Reconnect: NNG redials a dropped connection by itself (NNG_OPT_RECONNMINT/MAXT, defaults). The
 * worker's FO3DReconnectPolicy only paces reopening a socket that does not exist: an open, listen
 * or dial that failed, or a socket NNG reported closed. The two never run at once.
 *
 * Threading: Initialize/Start/Stop/Tick/CreateAudioSink: game thread. Send, SendSerialized,
 * SendControl, GetStats: any thread, never block.
 */
class FO3DNngSender : public IOpen3DSender
{
public:
    FO3DNngSender();
    virtual ~FO3DNngSender() override;

    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
    virtual FO3DTransportResult Start() override;
    virtual void Stop() override;
    virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    /** Depends on the mode the sender was initialized with (pub: Unreliable; pair, push: ReliableOrdered). */
    virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DNNG::GetCapabilitiesForMode(CapabilityMode.load()); }
    /**
     * Connecting until the first peer pipe exists, Connected while one does, Reconnecting after the
     * last went away. The worker thread reports pipe changes, within one worker wait (50 ms).
     */
    virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;
    virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;
    /** Called on an NNG thread for each added pipe (a subscriber, a peer, a pull socket; ADR 0005 (vi)). */
    virtual void SetPeerJoinedCallback(FO3DPeerJoinedCallback Callback) override;

    bool IsConnected() const { return PipeContext->bConnected.load(); }

    /**
     * Test hook: while paused the worker sends nothing, so the queue policy can be observed.
     * Pausing returns once the worker has seen the flag, so nothing enqueued afterwards is sent.
     */
    void SetWorkerPausedForTesting(bool bPaused);

private:
    struct FNngSocketWrapper;

    // Socket ownership (TRB-33): exactly one thread touches Socket at a time. Start() opens it
    // before the worker exists, the worker owns it (send, close, reopen) while it runs, and
    // Stop() closes it after joining the worker. Tick() never touches it.
    /** OutNngError receives the NNG error code of a failed open, listen or dial. */
    bool OpenSocket(int32* OutNngError = nullptr);
    void CloseSocket();
    /** Worker: reopens a missing socket when the reconnect policy allows. True if a socket is open. */
    bool EnsureSocketOnWorker();
    /** Worker: reports a change of "a peer pipe exists" to ConnectionState. */
    void UpdateConnectionStateOnWorker();
    /** Enqueues a frame; counts and logs a refusal (TRB-43). */
    EO3DSendResult EnqueueFrame(TArray<uint8>&& Bytes, FString Subject, double CaptureTimeSec, bool bFullSync);
    /** Empties the queue (worker not running). */
    void DrainQueue();

    // Worker thread only.
    uint32 RunWorkerIteration();
    /** Counts and logs a failed nng_send; closes the socket if NNG reports it closed. */
    void HandleSendError(int ErrorCode, bool bFrame);
    void RecordSendDrop();
    FString ResolveAudioSubjectFallback() const;

    O3DNNG::FNngSenderOptions Options;
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FGuid AudioSourceGuid;

    std::atomic<bool> bInitialized{ false };
    std::atomic<bool> bRunning{ false };
    std::atomic<bool> bWorkerPausedForTesting{ false };
    /** Worker iterations that saw bWorkerPausedForTesting; SetWorkerPausedForTesting waits on it. */
    std::atomic<int64> PausedIterations{ 0 };

    FNngSocketWrapper* Socket = nullptr;

    const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue;
    /** Shared with the audio sinks. Its peer flag stays true: NNG drops audio for absent peers itself. */
    const TSharedRef<FO3DAudioPublishState, ESPMode::ThreadSafe> PublishState;
    FO3DTransportWorker Worker;
    /** Worker (or Start, before the worker runs): paces reopening the socket. */
    FO3DReconnectPolicy ReopenPolicy;

    TSharedRef<FNngSenderPipeContext, ESPMode::ThreadSafe> PipeContext;
    /** Opaque nng_pipe_notify user data; resolves to PipeContext until the destructor. */
    void* PipeToken = nullptr;

    /**
     * The runtime context from the config (ADR 0012 item 3), and this transport's counters in
     * it, resolved in Initialize (SHR-3, SHR-17): no lock or lookup per frame. The default
     * context until then.
     */
    FO3DRuntimeContextRef Context;
    FO3DTransportMetricsRef TransportMetrics;
    /** Sender metrics: the config's handle, or this transport's own (ADR 0012 item 4). */
    FO3DSenderMetricsHandleRef SenderMetrics;

    std::atomic<int64> FramesSent{ 0 };
    std::atomic<int64> BytesSent{ 0 };
    /** Frames refused by the queue, dropped by the worker (no peer, NNG buffer full) or failed. */
    std::atomic<int64> DroppedFrames{ 0 };
    std::atomic<int64> SendErrors{ 0 };

    // Owned by whichever thread owns Socket (see above).
    double LastErrorLogTimestamp = 0.0;
    double LastDropLogTimestamp = 0.0;
    int64 DropsSinceLastLog = 0;
    /** Written by any thread that calls Send/SendSerialized. */
    std::atomic<double> LastBackpressureLogTimestamp{ 0.0 };
    /** nng.qmax as of the last Initialize, for the log line. */
    std::atomic<uint64> QueueLimitBytes{ 0 };

    /** Mode the capabilities are reported for: Options.Mode as of the last Initialize. */
    std::atomic<O3DNNG::ENngMode> CapabilityMode{ O3DNNG::ENngMode::Pub };

    /** ADR 0007 item 3. Peer changes are reported by the worker (UpdateConnectionStateOnWorker). */
    FO3DConnectionStateTracker ConnectionState;
    /** Worker only: whether a peer pipe existed at the last check. */
    bool bWorkerSawPeer = false;
};
