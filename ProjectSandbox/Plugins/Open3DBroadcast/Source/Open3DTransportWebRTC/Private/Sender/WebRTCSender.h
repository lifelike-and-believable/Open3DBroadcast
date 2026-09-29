#pragma once

#include "O3DSenderInterface.h"
#include "O3DTransportTypes.h"
#include "O3DLifetimeGate.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"

// Include LiveKit FFI for callback types
#include "livekit_ffi.h"

// Token management
#include "../Shared/WebRTCTokenManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DWebRTCSender, Log, All);

// Note: LiveKit FFI handles Opus encoding internally.
// We only need to provide PCM16 audio via lk_publish_audio_pcm_i16().

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
    TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();
    LkClientHandle* ClientHandle = nullptr;

    // Per-subject audio tracks (labeled audio publishing), keyed by StreamLabel.
    FCriticalSection AudioTracksMutex;
    TMap<FString, LkAudioTrackHandle*> AudioTracks;

    TAtomic<bool> bConnected{ false };
    // PHASE 10: estimated frames waiting in the LiveKit FFI queue (reset on connect).
    TAtomic<int32> EstimatedPendingFrames{ 0 };
};

/**
 * WebRTC transport sender implementation using LiveKit FFI.
 * Adapts the LiveKit FFI C ABI to the IOpen3DSender interface.
 */
class FO3DWebRTCSender : public IOpen3DSender
{
public:
    FO3DWebRTCSender();
    virtual ~FO3DWebRTCSender() override;

    // IOpen3DSender interface
    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual bool Send(const O3DS::SubjectList& List) override;
    virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
    // Configuration
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FString RoomUrl;
    FString Token;
    bool bPreferLossyData = false;

    // LiveKit FFI client handle (opaque)
    LkClientHandle* ClientHandle = nullptr;

    // WP-S5: audio tracks, connection flag and backpressure estimate live in Link, which audio
    // sinks and the connection callback share instead of referencing this sender.
    TSharedRef<FWebRTCSenderLink, ESPMode::ThreadSafe> Link;
    /** Opaque user data for lk_set_connection_callback; resolves to Link until the destructor. */
    void* LinkToken = nullptr;

    // State
    mutable FCriticalSection StateMutex;
    TAtomic<bool> bInitialized{ false };

    // Stats
    mutable FCriticalSection StatsMutex;
    FO3DTransportStats Stats;

    // PHASE 1: Serialization pool (sender frame thread only)
    TUniquePtr<class FSerializerPool> SerializerPool;

    // PHASE 10: WebRTC FFI Backpressure Monitoring
    // Tracks estimated pending frames in the LiveKit FFI queue to detect and adapt to network slowdowns
    // This prevents latency spikes by dropping frames when the FFI buffer backs up
    TAtomic<int64> LastFrameSendTimeUs{ 0 };       // Last send time for frame rate calculation
    TAtomic<int32> RecentSendRateFps{ 30 };        // Moving average frame rate (FPS)
    double LastBackpressureDecayTimeSeconds = 0.0; // For periodic queue depth decay

    // Backpressure thresholds (configurable via console variables)
    static constexpr int32 DefaultBackpressureThreshold = 30;  // Drop frames if >30 pending
    static constexpr int32 DefaultBackpressureDecayRate = 10;  // Assume 10 frames consumed per decay interval

    bool ShouldDropFrameDueToBackpressure() const;
    void UpdateFrameSendMetrics(int32 SubjectsInFrame);
    bool SendBytes(const uint8* Data, int32 Len, const FString& SubjectName);

    // Token management (auto-fetch support)
    TUniquePtr<FO3DTokenManager> TokenManager;
    TAtomic<bool> bWaitingForToken{ false };
    double TokenFetchStartTime = 0.0;
    static constexpr double TokenFetchTimeoutSec = 30.0;

    // Helper methods
    bool ParseConfig(const FO3DTransportConfig& Config);
    bool EnsureTokenAvailable();
    void CheckTokenRefresh();

    // LiveKit FFI callbacks (static)
    struct FCallbacks;
    static void OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message);
};
