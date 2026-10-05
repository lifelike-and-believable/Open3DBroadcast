// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DTransportWorker.h"
#include "Transport/O3DUnifiedReceiveDemux.h"
#include "Shared/NngHelpers.h"

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

/**
 * NNG receiver on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4d).
 *
 * Poll() (game thread, at most FramesPerPoll messages per call) takes what NNG's own I/O threads
 * already received (nng_recv with NNG_FLAG_NONBLOCK) and hands each message to the shared
 * FO3DUnifiedReceiveDemux, which calls the consumer, the audio sink and the control sink. NNG does
 * the socket I/O and the reconnecting of a dropped dialer on its own threads, so a transport
 * worker would only add a second queue; reopening a socket that failed to dial is paced with
 * FO3DReconnectPolicy.
 */
class FO3DNngReceiver : public IOpen3DReceiver
{
public:
    FO3DNngReceiver();
    virtual ~FO3DNngReceiver() override;

    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override { Demux.SetConsumer(InConsumer); }
    virtual FO3DTransportResult Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    /** Depends on the mode the receiver was initialized with (sub: Unreliable; pair, pull: ReliableOrdered). */
    virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DNNG::GetCapabilitiesForMode(CapabilityMode.load()); }
    /** Connecting until the first peer pipe exists, Connected while one does, Reconnecting after the last went away. Changes are reported from Poll (game thread). */
    virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
    virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { Demux.SetControlSink(Sink); }

    bool IsConnected() const { return PipeContext->bConnected.load(); }

private:
    // Test-only access to ProcessReceivedPayload (TRB-37 demux test); defined in
    // Private/Testing/NngTesting.cpp. Unconditional: friends must not depend on test macros.
    friend struct FO3DNngReceiverTestAccessor;

    struct FNngSocketWrapper;

    /** OutNngError receives the NNG error code of a failed open, listen or dial. */
    bool OpenSocket(int32* OutNngError = nullptr);
    void CloseSocket();
    /** Poll (game thread): reports a change of "a peer pipe exists" to ConnectionState. */
    void UpdateConnectionState();
    void HandleReceiveError(int ErrorCode);
    /** Reopens a missing socket when the reconnect policy allows. True if a socket is open. */
    bool EnsureSocket();
    /**
     * One received message through the demux. True for mocap and audio. Counts nothing: Poll
     * counts every message once, from OutResult (TRB-42).
     */
    bool ProcessReceivedPayload(const uint8* Data, int32 Size, EO3DDemuxResult* OutResult = nullptr);

    O3DNNG::FNngReceiverOptions Options;
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;

    /** Holds the consumer, audio sink and control sink strongly; Stop() releases them (TRF-38, ADR 0011). */
    FO3DUnifiedReceiveDemux Demux;

    FNngSocketWrapper* Socket = nullptr;

    std::atomic<bool> bInitialized{ false };
    std::atomic<bool> bRunning{ false };

    TSharedRef<FNngReceiverPipeContext, ESPMode::ThreadSafe> PipeContext;
    /** Opaque nng_pipe_notify user data; resolves to PipeContext until the destructor. */
    void* PipeToken = nullptr;

    // Game thread only (Start, Stop, Poll); pipe callbacks never touch these.
    FO3DReconnectPolicy ReopenPolicy;
    double LastErrorLogTimestamp = 0.0;
    constexpr static int32 FramesPerPoll = 16; // adjust to the polling budget you expect per tick

    // Written on the game thread, read by GetStats on any thread.
    std::atomic<int64> FramesReceived{ 0 };
    std::atomic<int64> BytesReceived{ 0 };
    std::atomic<int64> ReceiveErrors{ 0 };

    /** Mode the capabilities are reported for: Options.Mode as of the last Initialize. */
    std::atomic<O3DNNG::ENngMode> CapabilityMode{ O3DNNG::ENngMode::Sub };
    /** ADR 0007 item 3. */
    FO3DConnectionStateTracker ConnectionState;
    /** Game thread (Poll): whether a peer pipe existed at the last check. */
    bool bSawPeer = false;
};
