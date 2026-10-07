// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "Transport/O3DSenderInterface.h"
#include "O3DLogThrottle.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DLifetimeGate.h"
#include "O3DPerformanceMetrics.h"
#include "O3DRuntimeContext.h"
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

DECLARE_LOG_CATEGORY_EXTERN(LogO3DWebRTCSender, Log, All);

// Note: LiveKit FFI handles Opus encoding internally.
// We only need to provide PCM16 audio via lk_audio_track_publish_pcm_i16().

/**
 * State shared between FO3DWebRTCSender, its audio sinks and the LiveKit connection callback
 * (ADR 0007 addendum, WP-S5: TRF-1). Never holds a sender pointer.
 *
 * - Audio sinks enter Gate before touching ClientHandle or AudioTracks. Stop() closes the gate
 *   (waiting for any publish in flight) before it destroys tracks and the client.
 * - ClientHandle is written by the game thread only while the gate is closed.
 * - The connection callback receives an opaque token that resolves to this object, so it can
 *   never reach a destroyed sender. Its destructor does no FFI work.
 */
struct FWebRTCSenderLink
{
    explicit FWebRTCSenderLink(const FLkFfiApi& InFfi)
        : Ffi(InFfi)
    {
    }

    /** Copy of the owning sender's function table (immutable). */
    const FLkFfiApi Ffi;

    TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();
    LkClientHandle* ClientHandle = nullptr;

    // Per-subject audio tracks (labeled audio publishing), keyed by StreamLabel.
    FCriticalSection AudioTracksMutex;
    TMap<FString, LkAudioTrackHandle*> AudioTracks;
    /**
     * Tracks of a room the sender closed to reconnect (ADR 0015): livekit_ffi dropped their
     * pipelines, so a publish on one fails, but a sink may still hold one, so they are destroyed
     * only in Stop, after the gate closes. Guarded by AudioTracksMutex.
     */
    TArray<LkAudioTrackHandle*> RetiredAudioTracks;

    TAtomic<bool> bConnected{ false };

    /**
     * Latest LkConnectionState from the connection callback, -1 before the first one. The
     * sender's Tick turns it into connection-state changes (ADR 0007 item 3), so the callback
     * never reaches the sender or the state callback.
     */
    std::atomic<int32> LkState{ -1 };
    /** reason_code of that callback. */
    std::atomic<int32> LkReasonCode{ 0 };

    /**
     * The sender's transport counters, for the connection callback (ADR 0012 item 3). Set by
     * Initialize before it registers the callback, and not changed while one can run.
     */
    TSharedPtr<FO3DTransportMetrics, ESPMode::ThreadSafe> TransportMetrics;
};

/**
 * WebRTC transport sender implementation using LiveKit FFI.
 * Adapts the LiveKit FFI C ABI to the IOpen3DSender interface.
 *
 * Threading rule (WP-S7, TRF-15):
 * - Initialize, Start, Stop, Tick and CreateAudioSink are called on the game thread (the
 *   transport controller's thread). Connection and token state (bConnectRequested,
 *   bConnectIssued, token generations, retry times) is touched only there.
 * - Send and SendSerialized may be called from any thread. They read only Link->bConnected
 *   (atomic), ClientHandle (written on the game thread before the gate opens and after it
 *   closes) and Stats (StatsMutex).
 * - SendControl may be called from any thread. Like the audio sinks it enters Link->Gate, so
 *   Stop() cannot destroy the client during the send, and it touches no frame counter.
 * - LiveKit callbacks run on FFI threads and touch only the shared Link (atomics).
 * - Token fetch results never write sender members: FO3DTokenManager stores them under its own
 *   lock and Tick() reads them (TRF-3).
 *
 * The LiveKit C API is reached only through the FLkFfiApi table passed at construction, so
 * tests can run the sender against a fake without a LiveKit server (ADR 0006 F2).
 */
class FO3DWebRTCSender : public IOpen3DSender
{
public:
    /** Production constructor: uses the table linked from livekit_ffi.dll. */
    FO3DWebRTCSender();

    /**
     * Constructs a sender that calls LiveKit through InFfi, and fetches tokens with fetchers
     * from InTokenFetcherFactory (null uses the HTTP fetcher). InClock replaces
     * FPlatformTime::Seconds() for the reconnect timing, so tests need no sleeps. Used by tests.
     */
    explicit FO3DWebRTCSender(const FLkFfiApi& InFfi, FO3DTokenFetcherFactory InTokenFetcherFactory = nullptr, TFunction<double()> InClock = nullptr);

    virtual ~FO3DWebRTCSender() override;

    FO3DWebRTCSender(const FO3DWebRTCSender&) = delete;
    FO3DWebRTCSender& operator=(const FO3DWebRTCSender&) = delete;

    // IOpen3DSender interface
    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
    virtual FO3DTransportResult Start() override;
    virtual void Stop() override;
    /**
     * NotRunning before Start or after Stop, NotConnected while LiveKit is not connected (both
     * counted as dropped frames, as before), TooLarge above WebRTCUtils::ReliableMaxDataBytes,
     * DroppedBackpressure when lk_send_data_ex refuses the message.
     */
    virtual EO3DSendResult SendSerialized(FO3DSendPayload&& Payload) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    /** WebRTCUtils::GetCapabilities for the initialized config (Unreliable with webrtc.prefer_lossy). */
    virtual FO3DTransportCapabilities GetCapabilities() const override;
    /**
     * Connecting until LiveKit connects, Connected, Reconnecting while LiveKit reconnects and while
     * the sender retries, with backoff, after LiveKit gave up or a connect failed (ADR 0015).
     * LiveKit reports on its own threads; Tick (game thread) applies the change, so callbacks run
     * on the game thread.
     */
    virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

    /**
     * Control channel (docs/adr/0011-control-channel.md, CTL-6). Sends one control envelope on
     * the reliable, ordered data channel labelled `__o3d.ctl` (WebRTCUtils::ControlDataLabelUtf8).
     * NotRunning before Start or after Stop, Invalid for bytes that are not exactly one
     * well-formed control envelope within the 1,100-byte budget, NotConnected while
     * LiveKit is not connected (or reconnects), DroppedBackpressure when LiveKit refuses it; the
     * control publisher retries. Never counted as a frame, a sent byte or a dropped frame.
     */
    virtual EO3DSendResult SendControl(const uint8* Envelope, int32 Len) override;

private:
    /** Immutable after construction. */
    const FLkFfiApi Ffi;
    FO3DTokenFetcherFactory TokenFetcherFactory;
    TFunction<double()> Clock;
    /** FPlatformTime::Seconds(), or the test's clock. */
    double Now() const { return Clock ? Clock() : FPlatformTime::Seconds(); }

    // Configuration
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FString RoomUrl;
    bool bPreferLossyData = false;

    // LiveKit FFI client handle (opaque)
    LkClientHandle* ClientHandle = nullptr;

    // WP-S5: audio tracks and connection flag live in Link, which audio sinks and the
    // connection callback share instead of referencing this sender.
    TSharedRef<FWebRTCSenderLink, ESPMode::ThreadSafe> Link;
    /** Opaque user data for lk_set_connection_callback; resolves to Link until the destructor. */
    void* LinkToken = nullptr;

    /**
     * The runtime context from the config (ADR 0012 item 3), and this transport's counters in
     * it, resolved in Initialize (SHR-3, SHR-17): no lock or lookup per frame. The default
     * context until then.
     */
    FO3DRuntimeContextRef Context;
    FO3DTransportMetricsRef TransportMetrics;
    /** Sender metrics: the config's handle, or this transport's own (ADR 0012 item 4). */
    FO3DSenderMetricsHandleRef SenderMetrics;
    // WP-R3 (TR-6): hot-path log site, per instance.
    FO3DLogThrottle WebRTCTooLargeLog;

    // State
    mutable FCriticalSection StateMutex;
    TAtomic<bool> bInitialized{ false };

    // Stats
    mutable FCriticalSection StatsMutex;
    FO3DTransportStats Stats;

    EO3DSendResult SendBytes(const uint8* Data, int32 Len, const FString& SubjectName);

    /** Set by a successful Start, cleared by Stop. Any thread. */
    std::atomic<bool> bRunning{ false };
    /** webrtc.prefer_lossy of the initialized config, for GetCapabilities on any thread. */
    std::atomic<bool> bLossyCapabilities{ false };
    /** ADR 0007 item 3. Changed on the game thread (Start, Stop, Tick). */
    FO3DConnectionStateTracker ConnectionState;
    /** Game thread: the Link->LkState value last applied, and whether LiveKit connected in this session. */
    int32 AppliedLkState = -1;
    bool bEverConnected = false;
    /** Game thread (Tick): applies Link->LkState to ConnectionState. */
    void UpdateConnectionState();

    /** Control has its own log throttle, so its failures never hide a mocap message. Any thread. */
    std::atomic<double> LastControlErrorLogTime{ 0.0 };
    void RecordDroppedFrame();

    // Connection and token state (game thread only, TRF-3/TRF-15/TRF-23).
    TUniquePtr<FO3DTokenManager> TokenManager;
    /** Start() was called and Stop() was not. */
    bool bConnectRequested = false;
    /** lk_connect_with_role_async succeeded for this session. */
    bool bConnectIssued = false;
    /** Token generation last handed to LiveKit (connect or lk_refresh_token). */
    uint64 AppliedTokenGeneration = 0;
    /** Token generation last observed; a new one clears the fetch retry delay. */
    uint64 ObservedTokenGeneration = 0;
    double TokenFetchStartTime = 0.0;
    double NextTokenFetchTime = 0.0;
    static constexpr double TokenFetchTimeoutSec = 30.0;
    static constexpr double TokenFetchRetryIntervalSec = 5.0;
    /** ADR 0015: delay before reconnecting after LiveKit gave up or a connect failed; reset when it connects. */
    FO3DReconnectPolicy ReconnectPolicy{ WebRTCUtils::MakeReconnectPolicySettings() };
    /**
     * livekit_ffi still holds the room: from Connected until lk_disconnect, also after the SDK gave
     * up (Disconnected). A connect returns 104 "already connected" while it does.
     */
    bool bRoomHeld = false;

    // Helper methods
    FO3DTransportResult ParseConfig(const FO3DTransportConfig& Config);
    /** Drives token fetch, connect and token refresh. Game thread. Returns false if a connect call failed. */
    bool UpdateConnection();
    void MaybeFetchToken(double NowSeconds);
    /** Game thread: lk_disconnect on a room livekit_ffi still holds, and retires its audio tracks (ADR 0015). */
    void CloseHeldRoom();
    bool TryConnect(const FString& InToken, uint64 TokenGeneration, double NowSeconds);
    void ApplyRefreshedToken(const FString& InToken, uint64 TokenGeneration);

    // LiveKit FFI callbacks (static)
    static void OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message);
};
