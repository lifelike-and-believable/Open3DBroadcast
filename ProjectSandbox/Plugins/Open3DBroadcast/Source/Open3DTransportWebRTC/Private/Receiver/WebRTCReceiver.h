#pragma once

#include "O3DReceiverInterface.h"
#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"

// Include LiveKit FFI for callback types
#include "livekit_ffi.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DWebRTCReceiver, Log, All);

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

    // Per-subject frame buffering
    // With multiple labeled senders, we buffer frames per subject to avoid data loss
    // when multiple subjects transmit simultaneously.
    struct FPendingFrame
    {
        TArray<uint8> Payload;
        double EnqueueTimeSeconds = 0.0;

        // Pre-allocate buffer to typical mocap frame size to reduce GC pressure
        void ReserveForTypicalFrame()
        {
            static constexpr int32 TypicalFrameSizeBytes = 15 * 1024;  // ~15KB typical mocap frame
            if (Payload.Max() < TypicalFrameSizeBytes)
            {
                Payload.Reserve(TypicalFrameSizeBytes);
            }
        }
    };
    mutable FCriticalSection PendingFramesMutex;
    // Map from subject label to queue of pending frames
    TMap<FString, TArray<FPendingFrame>> PendingFramesBySubject;

    // Subject label cache - avoids repeated C-string→FString conversions (optimization)
    mutable FCriticalSection LabelCacheMutex;
    TMap<uint32, FString> SubjectLabelCache;

    // Stats / diagnostics
    mutable FCriticalSection StatsMutex;
    mutable FO3DTransportStats Stats;  // Mutable to allow updates in const GetStats() method

    // Atomic statistics for lock-free updates (high-frequency updates)
    TAtomic<int64> AtomicFramesReceived{ 0 };
    TAtomic<int64> AtomicFramesSent{ 0 };
    TAtomic<int64> AtomicBytesReceived{ 0 };
    TAtomic<int64> AtomicBytesSent{ 0 };
    TAtomic<int64> AtomicDroppedFrames{ 0 };

    int64 LatencySamples = 0;
    TAtomic<bool> bPendingAudioFormatApply{ false };
    mutable FCriticalSection LastDataMutex;
    double LastDataReceiveTime = 0.0;
    double NoDataReconnectTimeoutSec = 5.0;
    TAtomic<bool> bReconnectPending{ false };

    // Helper methods
    bool ParseConfig(const FO3DTransportConfig& Config);
    bool SetupClientHandle();
    bool BeginConnect();
    void ApplyPendingAudioFormatIfNeeded();
    void ProcessReconnectIfNeeded();
    void RequestReconnect(bool bForce = false);
    FString GetOrCacheSubjectLabel(const char* RawLabel);  // Cache C-string→FString conversions

    // LiveKit FFI callbacks (static)
    struct FCallbacks;
    static void OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message);
    static void OnDataReceivedEx(void* user, const char* label, LkReliability reliability, const uint8_t* bytes, size_t len);
    // Fallback: Unlabeled data callback (if labeled channels not supported)
    static void OnDataReceived(void* user, const uint8_t* bytes, size_t len);
    static void OnAudioReceived(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate);
    // New: Per-subject audio callback with participant and track names from LiveKit FFI
    static void OnAudioReceivedEx(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate, const char* participant_name, const char* track_name);

    double LastAudioDropLogTime = 0.0;
};
