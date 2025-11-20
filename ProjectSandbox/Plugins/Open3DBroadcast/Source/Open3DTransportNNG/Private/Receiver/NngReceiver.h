// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/ScopeLock.h"

#include "O3DReceiverInterface.h"
#include "Shared/NngHelpers.h"
#include "O3DAudioFrameCodec.h"

class FO3DNngReceiver : public IOpen3DReceiver
{
public:
    virtual ~FO3DNngReceiver() override = default;

    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;

    bool IsConnected() const { return bConnected.Load(); }

    void HandlePipeAdded();
    void HandlePipeRemoved();

private:
    struct FNngSocketWrapper;

    bool OpenSocket();
    void CloseSocket();
    void HandleReceiveError(int ErrorCode);
    bool EnsureDialSocket();
    bool ProcessReceivedPayload(const uint8* Data, int32 Size);
    bool ProcessAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize);

    O3DNNG::FNngReceiverOptions Options;
    FO3DTransportStats Stats;
    mutable FCriticalSection StatsMutex;
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;

    TWeakPtr<ISerializedFrameConsumer> Consumer;
    TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
    O3DAudio::FFrameDecoder AudioDecoder;
    TArray<int16> DecodedPcmScratch;

    FNngSocketWrapper* Socket = nullptr;

    TAtomic<bool> bInitialized{ false };
    TAtomic<bool> bRunning{ false };
    TAtomic<bool> bConnected{ false };

    FThreadSafeCounter PipeCount;

    double LastDialAttempt = 0.0;
    int32 BackoffAttempt = 0;
    double LastErrorLogTimestamp = 0.0;
    constexpr static int32 FramesPerPoll = 16; // adjust to the polling budget you expect per tick
    
};
