// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DTransportTypes.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DUnifiedReceiveDemux.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"
#include <atomic>

// Include LiveKit FFI for callback types
THIRD_PARTY_INCLUDES_START
#include "livekit_ffi.h"
THIRD_PARTY_INCLUDES_END

// Per-instance LiveKit function table (ADR 0006 F2)
#include "../Shared/LiveKitFfiApi.h"

// Token management
#include "../Shared/WebRTCTokenManager.h"
#include "../Shared/WebRTCUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DWebRTCReceiver, Log, All);

// Note: LiveKit FFI handles Opus decoding internally.
// We receive PCM16 audio directly via the audio callback.

/**
 * State shared between FO3DWebRTCReceiver and its LiveKit callbacks (WP-S7, TRF-15; replaces the
 * WP-S5 `this` exception). Never holds a receiver pointer.
 *
 * LiveKit callbacks receive an opaque token from TO3DFfiContextRegistry that resolves to this
 * object, so a callback that arrives after the receiver is gone does nothing. Everything here is
 * either atomic or guarded by its own lock, and the destructor frees only memory: the audio sink's
 * destructor does no FFI or UObject work (WP-S5), so an FFI thread may drop the last reference.
 */
struct FWebRTCReceiverLink
{
    FWebRTCReceiverLink();

    TAtomic<bool> bConnected{ false };
    /** Latest LkConnectionState from the connection callback, -1 before the first one; Poll applies it (ADR 0007 item 3). */
    std::atomic<int32> LkState{ -1 };
    /** reason_code of that callback. */
    std::atomic<int32> LkReasonCode{ 0 };
    TAtomic<bool> bPendingAudioFormatApply{ false };
    TAtomic<bool> bReconnectPending{ false };

    /** FPlatformTime::Seconds() of the last data message (or connect). */
    std::atomic<double> LastDataReceiveTime{ 0.0 };
    /** Rate limit for the "no audio sink" log; written from FFI threads. */
    std::atomic<double> LastAudioDropLogTime{ 0.0 };

    TAtomic<int64> FramesReceived{ 0 };
    TAtomic<int64> BytesReceived{ 0 };

    /**
     * Control envelopes (ADR 0011) waiting for Poll(), in arrival order: an FO3DSendQueue of
     * control items (cap 1,024, the newest refused), filled on FFI threads and drained by Poll on
     * the game thread (WP-A1 PR 4f). Separate from the frames, so control never reaches the frame
     * consumer or moves a frame counter.
     */
    FO3DSendQueue ControlQueue;
    /** True while the receiver has a control sink; the data callback drops control otherwise. */
    TAtomic<bool> bControlWanted{ false };
    /** Rate limit for control drop logs, separate from every frame log; written from FFI threads. */
    std::atomic<double> LastControlDropLogTime{ 0.0 };

    /**
     * Data-channel frames waiting for Poll(), in arrival order, each with its decoded label as the
     * subject: an FO3DSendQueue of mocap items (RefuseNewest, MaxPendingFrameBytes), filled on FFI
     * threads and drained by Poll on the game thread (WP-A1 PR 4f; it used to be unbounded).
     */
    FO3DSendQueue FrameQueue;
    /** Frames refused because FrameQueue was full; reported in DroppedFrames. */
    std::atomic<int64> FramesRefused{ 0 };
    static constexpr int64 MaxPendingFrameBytes = 16ll * 1024ll * 1024ll;

    /** Written on the game thread, copied by audio callbacks. Guarded by AudioSinkMutex only. */
    FCriticalSection AudioSinkMutex;
    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;

    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> GetAudioSink()
    {
        FScopeLock Lock(&AudioSinkMutex);
        return AudioSink;
    }

    void EnqueueFrame(const FString& SubjectLabel, const uint8* Bytes, int32 Len);
    /**
     * Data callback classification (ADR 0011 item 7), run before EnqueueFrame whatever the label.
     * Returns false when the bytes are not a control-kind envelope (they are mocap). Returns true
     * when they are, after queueing a well-formed envelope for Poll() or dropping it (no sink,
     * malformed, queue full); either way the bytes must not be treated as mocap.
     */
    bool ConsumeControl(const uint8* Bytes, size_t Len);
    void LogControlDrop(const TCHAR* Reason);
    void RequestReconnect();
};

/**
 * WebRTC transport receiver implementation using LiveKit FFI.
 * Adapts the LiveKit FFI C ABI to the IOpen3DReceiver interface.
 *
 * Threading rule (WP-S7, TRF-15):
 * - Initialize, SetConsumer, SetAudioSink, Start, Stop and Poll are called on the game thread
 *   (FO3DReceiverSource::Tick and its start/stop paths). Connection and token state
 *   (ClientHandle, bConnectRequested, bConnectIssued, token generations, Consumer) is touched
 *   only there, under StateMutex.
 * - LiveKit callbacks run on FFI threads and touch only the shared Link: atomics, the frame and
 *   control queues (FO3DSendQueue: lock-free producers, Poll the only consumer) and the audio
 *   sink (AudioSinkMutex). They never take StateMutex, so Stop() may hold it while
 *   lk_disconnect waits for callbacks to finish.
 * - Control (ADR 0011): the data callback classifies enveloped control bytes before the
 *   "label = subject" mocap path and queues them; Poll() hands them to the control sink through
 *   the shared receive demux on the game thread. The sink is set and released (in Stop) there.
 * - Token fetch results never write receiver members: FO3DTokenManager stores them under its
 *   own lock and Poll() reads them (TRF-3).
 * - GetStats may be called from any thread.
 *
 * The LiveKit C API is reached only through the FLkFfiApi table passed at construction, so
 * tests can run the receiver against a fake without a LiveKit server (ADR 0006 F2).
 */
class FO3DWebRTCReceiver : public IOpen3DReceiver
{
public:
    /** Production constructor: uses the table linked from livekit_ffi.dll. */
    FO3DWebRTCReceiver();

    /**
     * Constructs a receiver that calls LiveKit through InFfi, and fetches tokens with fetchers
     * from InTokenFetcherFactory (null uses the HTTP fetcher). Used by tests.
     */
    explicit FO3DWebRTCReceiver(const FLkFfiApi& InFfi, FO3DTokenFetcherFactory InTokenFetcherFactory = nullptr);

    virtual ~FO3DWebRTCReceiver() override;

    FO3DWebRTCReceiver(const FO3DWebRTCReceiver&) = delete;
    FO3DWebRTCReceiver& operator=(const FO3DWebRTCReceiver&) = delete;

    // IOpen3DReceiver interface
    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
    virtual FO3DTransportResult Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    /** WebRTCUtils::GetCapabilities; a receiver cannot tell whether the sender prefers lossy data, so it reports ReliableOrdered. */
    virtual FO3DTransportCapabilities GetCapabilities() const override { return WebRTCUtils::GetCapabilities(FO3DTransportConfig()); }
    /**
     * Connecting until LiveKit connects, Connected, Reconnecting after a drop (the receiver
     * reconnects by itself). LiveKit reports on its own threads; Poll (game thread) applies the
     * change, so callbacks run on the game thread.
     */
    virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
    /** Control channel (docs/adr/0011-control-channel.md, CTL-6). Game thread, before Start. Held strongly until Stop (or a later call) releases it. */
    virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override;

private:
    /** Immutable after construction. */
    const FLkFfiApi Ffi;
    FO3DTokenFetcherFactory TokenFetcherFactory;

    // Configuration
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FString RoomUrl;

    // LiveKit FFI client handle (opaque). Game thread only.
    LkClientHandle* ClientHandle = nullptr;

    /** Callback-shared state; created in the constructor and never reassigned. */
    TSharedRef<FWebRTCReceiverLink, ESPMode::ThreadSafe> Link;
    /** Opaque user data for every LiveKit callback; resolves to Link until the destructor. */
    void* LinkToken = nullptr;

    // State
    mutable FCriticalSection StateMutex;
    TAtomic<bool> bInitialized{ false };

    /**
     * Holds the consumer and the control sink strongly and delivers mocap (DeliverMocap, the data
     * label as the subject) and control (DeliverControlEnvelope); Stop() releases them (TRF-38,
     * ADR 0011). Game thread only. Audio does not go through it: LiveKit decodes Opus itself and
     * hands PCM16 to the audio callback, which calls the audio sink directly on its thread.
     */
    FO3DUnifiedReceiveDemux Demux;

    // Stats / diagnostics
    mutable FCriticalSection StatsMutex;
    mutable FO3DTransportStats Stats;  // Mutable to allow updates in const GetStats() method
    int64 LatencySamples = 0;

    double NoDataReconnectTimeoutSec = 5.0;

    // Connection and token state (game thread only, TRF-3/TRF-15/TRF-23).
    TUniquePtr<FO3DTokenManager> TokenManager;
    bool bConnectRequested = false;
    bool bConnectIssued = false;
    uint64 AppliedTokenGeneration = 0;
    uint64 ObservedTokenGeneration = 0;
    double TokenFetchStartTime = 0.0;
    double NextTokenFetchTime = 0.0;
    double NextConnectAttemptTime = 0.0;
    static constexpr double TokenFetchTimeoutSec = 30.0;
    static constexpr double TokenFetchRetryIntervalSec = 5.0;
    static constexpr double ConnectRetryIntervalSec = 5.0;

    // Helper methods
    FO3DTransportResult ParseConfig(const FO3DTransportConfig& Config);
    /** Game thread (Poll), StateMutex held: applies Link->LkState to ConnectionState. */
    void UpdateConnectionState();

    /** ADR 0007 item 3. Changed on the game thread (Start, Stop, Poll). */
    FO3DConnectionStateTracker ConnectionState;
    /** Game thread: the Link->LkState value last applied, and whether LiveKit connected in this session. */
    int32 AppliedLkState = -1;
    bool bEverConnected = false;
    bool SetupClientHandle();
    void DestroyClientHandle(const TCHAR* Context);
    bool BeginConnect(const FString& InToken, uint64 TokenGeneration);
    void ApplyPendingAudioFormatIfNeeded();
    void ProcessReconnectIfNeeded();
    /** Drives token fetch, connect and token refresh. StateMutex held. Returns false if a connect call failed. */
    bool UpdateConnection();
    void MaybeFetchToken(double NowSeconds);
    void ApplyRefreshedToken(const FString& InToken, uint64 TokenGeneration);

    // LiveKit FFI callbacks (static). `user` is the opaque LinkToken.
    static void OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message);
    static void OnDataReceivedEx(void* user, const char* label, LkReliability reliability, const uint8_t* bytes, size_t len);
    // Unlabeled data callback, registered with the labeled one: livekit_ffi's async-connect loop
    // calls only this one.
    static void OnDataReceived(void* user, const uint8_t* bytes, size_t len);
    // Per-subject audio callback with participant and track names from LiveKit FFI
    static void OnAudioReceivedEx(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate, const char* participant_name, const char* track_name);
};
