#pragma once

#include "O3DSenderInterface.h"
#include "O3DTransportTypes.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"

// Include LiveKit FFI for callback types
#include "livekit_ffi.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DWebRTCSender, Log, All);

// Note: LiveKit FFI handles Opus encoding internally.
// We only need to provide PCM16 audio via lk_publish_audio_pcm_i16().

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
    virtual void Tick(float DeltaSeconds) override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
    friend class FWebRTCSenderAudioSink;

    // Configuration
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FString RoomUrl;
    FString Token;
    bool bPreferLossyData = false;

    // LiveKit FFI client handle (opaque)
    LkClientHandle* ClientHandle = nullptr;

    // Per-subject audio tracks (labeled audio publishing)
    // Map from StreamLabel to audio track handle
    TMap<FString, LkAudioTrackHandle*> AudioTracks;
    mutable FCriticalSection AudioTracksMutex;

    // State
    mutable FCriticalSection StateMutex;
    TAtomic<bool> bInitialized{ false };
    TAtomic<bool> bConnected{ false };

    // Stats
    mutable FCriticalSection StatsMutex;
    FO3DTransportStats Stats;

    // Helper methods
    bool ParseConfig(const FO3DTransportConfig& Config);

    // LiveKit FFI callbacks (static)
    struct FCallbacks;
    static void OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message);
};
