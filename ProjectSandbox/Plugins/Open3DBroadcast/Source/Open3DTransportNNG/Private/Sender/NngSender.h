// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/ThreadSafeCounter.h"

#include "O3DSenderInterface.h"
#include "Shared/NngHelpers.h"
#include "O3DAudioFrameCodec.h"

class FO3DNngSender : public IOpen3DSender
{
public:
    virtual ~FO3DNngSender() override = default;

    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual bool Send(const O3DS::SubjectList& List) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

    void HandlePipeAdded();
    void HandlePipeRemoved();

private:
    struct FQueuedPayload
    {
        TArray<uint8> Bytes;
    };

    struct FNngSocketWrapper;
    class FNngSenderRunnable;
    class FNngSenderAudioSink;

    friend class FNngSenderRunnable;
    friend class FNngSenderAudioSink;

    bool OpenSocket();
    void CloseSocket();
    void StartWorker();
    void StopWorker();
    uint32 RunWorker();
    bool EnqueuePayload(const uint8* Data, int32 Size);
    void DrainQueue();
    void HandleSendError(int ErrorCode);
    void RefreshAudioEncoder();
    bool ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);
    bool SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec);

    mutable FCriticalSection StateMutex;
    mutable FCriticalSection StatsMutex;

    O3DNNG::FNngSenderOptions Options;
    FO3DTransportStats Stats;
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FGuid AudioSourceGuid;
    bool bAudioEncoderInitialized = false;
    O3DAudio::FFrameEncoder AudioEncoder;
    TArray<uint8> UnifiedAudioScratch;

    TAtomic<bool> bInitialized{ false };
    TAtomic<bool> bRunning{ false };
    TAtomic<bool> bStopWorker{ false };
    TAtomic<bool> bConnected{ false };

    FNngSocketWrapper* Socket = nullptr;

    FNngSenderRunnable* Worker = nullptr;
    FRunnableThread* WorkerThread = nullptr;
    FEvent* WakeEvent = nullptr;

    TQueue<FQueuedPayload, EQueueMode::Mpsc> Queue;
    TAtomic<uint64> QueueBytes{ 0 };

    FThreadSafeCounter PipeCount;

    double LastErrorLogTimestamp = 0.0;
    double LastBackoffAttemptTime = 0.0;
    int32 BackoffAttempt = 0;
    double LastBackpressureLogTimestamp = 0.0;
};
