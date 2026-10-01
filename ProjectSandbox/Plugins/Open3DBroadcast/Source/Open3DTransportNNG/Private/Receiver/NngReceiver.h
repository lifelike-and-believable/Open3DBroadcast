// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeLock.h"

#include "Transport/O3DReceiverInterface.h"
#include "Shared/NngHelpers.h"
#include "O3DAudioFrameCodec.h"

#include <atomic>

/**
 * Context for the receiver's NNG pipe-notify callback, reached through an opaque token
 * (TRB-42, same pattern as the sender, TRF-12). Holds atomics only, so an NNG thread may drop
 * the last reference and a late callback never touches the receiver.
 */
struct FNngReceiverPipeContext
{
    std::atomic<int32> PipeCount{0};
    std::atomic<bool> bConnected{false};
    /** True for a listening socket, which stays "connected" (ready) with no pipes. */
    std::atomic<bool> bConnectedWithoutPipes{false};
};

class FO3DNngReceiver : public IOpen3DReceiver
{
public:
    FO3DNngReceiver();
    virtual ~FO3DNngReceiver() override;

    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
    virtual bool SupportsControl() const override { return true; }
    virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { ControlSink = Sink; }

    bool IsConnected() const { return PipeContext->bConnected.load(); }

private:
    // Test-only access to ProcessReceivedPayload (TRB-37 demux test); defined in
    // Private/Testing/NngTesting.cpp. Unconditional: friends must not depend on test macros.
    friend struct FO3DNngReceiverTestAccessor;

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
    /** Control payloads (ADR 0011). Held strongly, released in Stop; used only from Poll (game thread). */
    TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ControlSink;
    O3DAudio::FMultiStreamFrameDecoder AudioDecoder; // SHR-15: one decoder per (SourceGuid, StreamLabel)
    TArray<int16> DecodedPcmScratch;

    FNngSocketWrapper* Socket = nullptr;

    TAtomic<bool> bInitialized{ false };
    TAtomic<bool> bRunning{ false };

    TSharedRef<FNngReceiverPipeContext, ESPMode::ThreadSafe> PipeContext;
    /** Opaque nng_pipe_notify user data; resolves to PipeContext until the destructor. */
    void* PipeToken = nullptr;

    // Game thread only (Start, Stop, Poll); pipe callbacks never touch these.
    double LastDialAttempt = 0.0;
    int32 BackoffAttempt = 0;
    double LastErrorLogTimestamp = 0.0;
    constexpr static int32 FramesPerPoll = 16; // adjust to the polling budget you expect per tick
    
};
