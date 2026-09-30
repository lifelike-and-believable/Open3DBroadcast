#pragma once

#include "O3DSenderInterface.h"
#include "O3DTransportTypes.h"
#include "O3DLifetimeGate.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"

// Include LiveKit FFI for callback types
THIRD_PARTY_INCLUDES_START
#include "livekit_ffi.h"
THIRD_PARTY_INCLUDES_END

// Per-instance LiveKit function table (ADR 0006 F2)
#include "../Shared/LiveKitFfiApi.h"

// Token management
#include "../Shared/WebRTCTokenManager.h"

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

    TAtomic<bool> bConnected{ false };
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
     * from InTokenFetcherFactory (null uses the HTTP fetcher). Used by tests.
     */
    explicit FO3DWebRTCSender(const FLkFfiApi& InFfi, FO3DTokenFetcherFactory InTokenFetcherFactory = nullptr);

    virtual ~FO3DWebRTCSender() override;

    FO3DWebRTCSender(const FO3DWebRTCSender&) = delete;
    FO3DWebRTCSender& operator=(const FO3DWebRTCSender&) = delete;

    // IOpen3DSender interface
    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual bool Start() override;
    virtual void Stop() override;
    /**
     * Serializes the whole list once and sends it under the first subject's name (TRF-4,
     * TRF-19). The normal pipeline uses SendSerialized; this path exists for the interface.
     */
    virtual bool Send(const O3DS::SubjectList& List) override;
    virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
    /** Immutable after construction. */
    const FLkFfiApi Ffi;
    FO3DTokenFetcherFactory TokenFetcherFactory;

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

    // State
    mutable FCriticalSection StateMutex;
    TAtomic<bool> bInitialized{ false };

    // Stats
    mutable FCriticalSection StatsMutex;
    FO3DTransportStats Stats;

    bool SendBytes(const uint8* Data, int32 Len, const FString& SubjectName);
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
    double NextConnectAttemptTime = 0.0;
    static constexpr double TokenFetchTimeoutSec = 30.0;
    static constexpr double TokenFetchRetryIntervalSec = 5.0;
    static constexpr double ConnectRetryIntervalSec = 5.0;

    // Helper methods
    bool ParseConfig(const FO3DTransportConfig& Config);
    /** Drives token fetch, connect and token refresh. Game thread. Returns false if a connect call failed. */
    bool UpdateConnection();
    void MaybeFetchToken(double NowSeconds);
    bool TryConnect(const FString& InToken, uint64 TokenGeneration, double NowSeconds);
    void ApplyRefreshedToken(const FString& InToken, uint64 TokenGeneration);

    // LiveKit FFI callbacks (static)
    static void OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message);
};
