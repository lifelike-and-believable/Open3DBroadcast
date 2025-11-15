#pragma once

#include "O3DReceiverInterface.h"
#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"

// Forward declare LiveKit FFI types
struct LkClientHandle;

DECLARE_LOG_CATEGORY_EXTERN(LogO3DWebRTCTransport, Log, All);

// Note: LiveKit FFI handles Opus decoding internally.
// We receive PCM16 audio directly via the audio callback.

/**
 * WebRTC transport receiver implementation using LiveKit FFI.
 * Adapts the LiveKit FFI C ABI to the IOpen3DReceiver interface.
 */
class FO3DWebRTCReceiver : public IOpen3DReceiver
{
public:
    FO3DWebRTCReceiver();
    virtual ~FO3DWebRTCReceiver() override;

    // IOpen3DReceiver interface
    virtual bool Initialize(const FO3DTransportConfig& Config) override;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
    virtual bool Start() override;
    virtual void Stop() override;
    virtual int32 Poll() override;
    virtual FO3DTransportStats GetStats() const override;
    virtual bool SupportsAudio() const override { return true; }
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;

private:
    // Configuration
    FO3DTransportConfig ActiveConfig;
    FO3DTransportAudioConfig ActiveAudioConfig;
    FString RoomUrl;
    FString Token;
    FString SubjectName;

    // LiveKit FFI client handle (opaque)
    LkClientHandle* ClientHandle = nullptr;

    // State
    mutable FCriticalSection StateMutex;
    TAtomic<bool> bInitialized{ false };
    TAtomic<bool> bConnected{ false };

    // Consumer
    TSharedPtr<ISerializedFrameConsumer> Consumer;
    TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;

    // Stats
    mutable FCriticalSection StatsMutex;
    FO3DTransportStats Stats;
    int64 LatencySamples = 0;

    // Helper methods
    bool ParseConfig(const FO3DTransportConfig& Config);
    void AccumulateLatency(double LatencyMs);

    // LiveKit FFI callbacks (static)
    struct FCallbacks;
    static void OnConnectionState(void* user, int32_t state, int32_t reason_code, const char* message);
    static void OnDataReceived(void* user, const uint8_t* bytes, size_t len);
    static void OnAudioReceived(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate);

    double LastAudioDropLogTime = 0.0;
};
