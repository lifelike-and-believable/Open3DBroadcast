// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

#include "Transport/O3DSenderInterface.h"
#include "Shared/NngHelpers.h"
#include "O3DAudioFrameCodec.h"
#include "O3DEncodedPayloadQueue.h"
#include "O3DLifetimeGate.h"
#include "O3DPerformanceMetrics.h"
#include "O3DSinkAudioEncoder.h"

#include <atomic>

class FRunnableThread;

/**
 * Publish state shared between FO3DNngSender, its worker and its audio sinks
 * (ADR 0007 addendum, WP-S5: TRB-10, TRB-12, TRB-35). No sender pointer, no socket.
 */
struct FNngSenderPublishState
{
    TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();
    /** Mocap and audio payloads for the worker; owns the worker's wake event. */
    FO3DEncodedPayloadQueue SendQueue;
    FO3DAudioSubjectSlot LastSubject;
    std::atomic<int64> AudioDropped{0};
};

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
};

class FO3DNngSender : public IOpen3DSender
{
public:
    FO3DNngSender();
    virtual ~FO3DNngSender() override;

    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual bool Send(const O3DS::SubjectList& List) override;
    virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

    bool IsConnected() const { return PipeContext->bConnected.load(); }

private:
    struct FNngSocketWrapper;
    class FNngSenderRunnable;

    friend class FNngSenderRunnable;

    // Socket ownership (TRB-33): exactly one thread touches Socket at a time. Start() opens it
    // before the worker exists, the worker owns it (send, close, reopen) while it runs, and
    // Stop() closes it after joining the worker. Tick() never touches it.
    bool OpenSocket();
    void CloseSocket();
    /** Worker: reopens a closed socket after the backoff delay. Returns true if a socket is open. */
    bool EnsureSocketOnWorker();
    bool SendBytes(const uint8* Data, int32 Len, const FString& SubjectName);
    void StartWorker();
    void StopWorker();
    uint32 RunWorker();
    bool EnqueuePayload(const uint8* Data, int32 Size);
    void DrainQueue();
    /** Worker: counts and logs a failed nng_send; closes the socket if NNG reports it closed. */
    void HandleSendError(int ErrorCode);
    void RecordSendDrop();
    FString ResolveAudioSubjectFallback() const;

    mutable FCriticalSection StateMutex;
    mutable FCriticalSection StatsMutex;

    O3DNNG::FNngSenderOptions Options;
    FO3DTransportStats Stats;
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FGuid AudioSourceGuid;

    TAtomic<bool> bInitialized{ false };
    TAtomic<bool> bRunning{ false };
    TAtomic<bool> bStopWorker{ false };

    FNngSocketWrapper* Socket = nullptr;

    FNngSenderRunnable* Worker = nullptr;
    FRunnableThread* WorkerThread = nullptr;

    TSharedRef<FNngSenderPublishState, ESPMode::ThreadSafe> PublishState;
    TSharedRef<FNngSenderPipeContext, ESPMode::ThreadSafe> PipeContext;
    /** Opaque nng_pipe_notify user data; resolves to PipeContext until the destructor. */
    void* PipeToken = nullptr;

    /** This transport's counters, resolved once (SHR-3, SHR-17): no lock or lookup per frame. */
    const FO3DTransportMetricsRef TransportMetrics;

    // Owned by whichever thread owns Socket (see above).
    double LastErrorLogTimestamp = 0.0;
    double LastDropLogTimestamp = 0.0;
    double LastBackoffAttemptTime = 0.0;
    int32 BackoffAttempt = 0;
    int64 DropsSinceLastLog = 0;
    /** Written by any thread that calls Send/SendSerialized. */
    std::atomic<double> LastBackpressureLogTimestamp{ 0.0 };
};
