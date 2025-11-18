#include "WebRTCSender.h"
#include "../Shared/WebRTCUtils.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "livekit_ffi.h"
#include "o3ds/model.h"
#include <vector>

using WebRTCUtils::FromAnsi;

namespace WebRTCOptions
{
    static constexpr TCHAR PreferLossyOptionKey[] = TEXT("webrtc.prefer_lossy");

    static bool ParseBool(const TMap<FString, FString>& Params, const TCHAR* Key, bool DefaultValue)
    {
        if (!Key)
        {
            return DefaultValue;
        }

        if (const FString* Value = Params.Find(Key))
        {
            if (Value->Equals(TEXT("1"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("yes"), ESearchCase::IgnoreCase))
            {
                return true;
            }

            if (Value->Equals(TEXT("0"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
                Value->Equals(TEXT("no"), ESearchCase::IgnoreCase))
            {
                return false;
            }
        }

        return DefaultValue;
    }
}

/**
 * Audio sink implementation for WebRTC sender using LiveKit FFI.
 *
 * Optimized with reusable PCM conversion buffer to avoid per-frame allocations.
 * This significantly reduces allocator pressure on the audio thread.
 */
class FWebRTCSenderAudioSink final : public IO3DSenderAudioSink
{
public:
    FWebRTCSenderAudioSink(FO3DWebRTCSender& InOwner)
        : Owner(InOwner), PcmConversionBuffer()
    {
    }

    virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
    {
        if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
        {
            return false;
        }

        if (!Owner.ClientHandle || !Owner.bConnected.Load())
        {
            return false;
        }

        // Reuse buffer, allocate only if necessary (optimization to reduce per-frame allocations)
        const int32 TotalSamples = NumFrames * NumChannels;
        if (PcmConversionBuffer.Num() < TotalSamples)
        {
            PcmConversionBuffer.SetNumUninitialized(TotalSamples);
        }

        // Convert float to int16 with clamping
        for (int32 i = 0; i < TotalSamples; ++i)
        {
            float Sample = FMath::Clamp(Interleaved[i], -1.0f, 1.0f);
            PcmConversionBuffer[i] = static_cast<int16>(Sample * 32767.0f);
        }

        // Publish to LiveKit
        LkResult Result = lk_publish_audio_pcm_i16(
            Owner.ClientHandle,
            PcmConversionBuffer.GetData(),
            NumFrames,
            NumChannels,
            SampleRate
        );

        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to publish audio: %s"), *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
            return false;
        }

        return true;
    }

    virtual void OnCaptureStopped() override
    {
        // No cleanup needed
    }

private:
    FO3DWebRTCSender& Owner;

    // Reusable buffer for float->int16 conversion (optimization: avoid per-frame allocation)
    // Typical size: 960 samples * 2 channels * 2 bytes = 3840 bytes
    TArray<int16> PcmConversionBuffer;
};

// Static callback for connection state changes
void FO3DWebRTCSender::OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message)
{
    FO3DWebRTCSender* Self = reinterpret_cast<FO3DWebRTCSender*>(user);
    if (!Self) return;

    switch (state)
    {
    case LkConnConnecting:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC connecting..."));
        break;

    case LkConnConnected:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC connected"));
        Self->bConnected.Store(true);
        break;

    case LkConnReconnecting:
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("WebRTC reconnecting..."));
        Self->bConnected.Store(false);
        break;

    case LkConnDisconnected:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC disconnected: %s"), *FromAnsi(message));
        Self->bConnected.Store(false);
        break;

    case LkConnFailed:
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC connection failed (code=%d): %s"), reason_code, *FromAnsi(message));
        Self->bConnected.Store(false);
        break;
    }
}

FO3DWebRTCSender::FO3DWebRTCSender()
{
    // LiveKit FFI DLL is loaded automatically via .lib linkage
}

FO3DWebRTCSender::~FO3DWebRTCSender()
{
    Stop();
}

bool FO3DWebRTCSender::Initialize(const FO3DTransportConfig& Config)
{
    FScopeLock Lock(&StateMutex);

    if (bInitialized.Load())
    {
        #if !WITH_DEV_AUTOMATION_TESTS
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC sender already initialized"));
        #endif
        return false;
    }

    // Platform validation: WebRTC module currently supports Windows 64-bit only
#if !PLATFORM_WINDOWS || !PLATFORM_64BITS
    UE_LOG(LogO3DWebRTCSender, Error,
        TEXT("Open3DTransportWebRTC requires Windows 64-bit. Current platform: %s %d-bit. "
             "Please use alternative transport (TCP, UDP, NNG, or Loopback) or compile LiveKit FFI for your platform."),
#if PLATFORM_WINDOWS
        TEXT("Windows"),
#elif PLATFORM_MAC
        TEXT("macOS"),
#elif PLATFORM_LINUX
        TEXT("Linux"),
#else
        TEXT("Unknown"),
#endif
        (int32)(sizeof(void*) * 8));
    return false;
#endif

    if (!ParseConfig(Config))
    {
        return false;
    }

    // Create LiveKit client handle
    ClientHandle = lk_client_create();
    if (!ClientHandle)
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to create LiveKit client"));
        return false;
    }

    // Set connection callback
    LkResult Result = lk_set_connection_callback(ClientHandle, FO3DWebRTCSender::OnConnectionState, this);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to set connection callback: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    // Configure audio if enabled
    if (Config.Audio.bEnableAudio)
    {
        ActiveAudioConfig = Config.Audio;

        int32 BitrateKbps = FMath::Clamp(ActiveAudioConfig.BitrateKbps, 16, 128);
        Result = lk_set_audio_publish_options(ClientHandle, BitrateKbps * 1000, 0, ActiveAudioConfig.NumChannels > 1 ? 1 : 0);

        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to set audio options: %s"), *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
        }
    }

    ActiveConfig = Config;
    Stats.Reset();
    bInitialized.Store(true);

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender initialized: URL=%s Audio=%s PreferLossy=%s"),
        *RoomUrl,
        ActiveAudioConfig.bEnableAudio ? TEXT("true") : TEXT("false"),
        bPreferLossyData ? TEXT("true") : TEXT("false"));

    return true;
}

bool FO3DWebRTCSender::Start()
{
    FScopeLock Lock(&StateMutex);

    if (!bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Cannot start WebRTC sender: not initialized"));
        return false;
    }

    if (bConnected.Load())
    {
        #if !WITH_DEV_AUTOMATION_TESTS
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC sender already connected"));
        #endif
        return true;
    }

    // Connect asynchronously with Publisher role
    LkResult Result = lk_connect_with_role_async(
        ClientHandle,
        TCHAR_TO_UTF8(*RoomUrl),
        TCHAR_TO_UTF8(*Token),
        LkRolePublisher
    );

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to connect: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
        return false;
    }

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender connecting..."));
    return true;
}

void FO3DWebRTCSender::Stop()
{
    FScopeLock Lock(&StateMutex);

    if (!ClientHandle)
    {
        return;
    }

    if (bConnected.Load())
    {
        LkResult Result = lk_disconnect(ClientHandle);
        if (Result.code != 0 && Result.message)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Disconnect warning: %s"), *FromAnsi(Result.message));
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    lk_client_destroy(ClientHandle);
    ClientHandle = nullptr;

    bConnected.Store(false);
    bInitialized.Store(false);

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender stopped"));
}

bool FO3DWebRTCSender::Send(const O3DS::SubjectList& List)
{
    if (!bConnected.Load())
    {
        static double LastDisconnectedWarningTime = 0.0;
        const double Now = FPlatformTime::Seconds();
        if (Now - LastDisconnectedWarningTime > 5.0)
        {
            UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("Send() called while not connected, dropping frame"));
            LastDisconnectedWarningTime = Now;
        }
        {
            FScopeLock Lock(&StatsMutex);
            Stats.DroppedFrames++;
        }
        return false;
    }

    // Serialize the SubjectList
    std::vector<char> Buffer;
    const double TimestampSeconds = FPlatformTime::Seconds();

    // Note: Serialize() is non-const, but doesn't modify the SubjectList in practice
    int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
    if (BytesWritten <= 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to serialize SubjectList"));
        return false;
    }

    // Validate payload size (LiveKit lossy DataChannel limit ~1300 bytes, reliable ~15KB)
    // See: https://docs.livekit.io/home/server/overview/concepts/data-channel-reliability
    constexpr int32 LossyMaxBytes = 1300;
    constexpr int32 ReliableMaxBytes = 15000;

    const bool bAllowLossy = bPreferLossyData;
    LkReliability Reliability = bAllowLossy ? LkLossy : LkReliable;

    if (bAllowLossy)
    {
        if (BytesWritten > LossyMaxBytes)
        {
            if (BytesWritten <= ReliableMaxBytes)
            {
                Reliability = LkReliable;
                UE_LOG(LogO3DWebRTCSender, Warning,
                    TEXT("Payload size (%d bytes) exceeds lossy limit (%d bytes), switching to reliable channel"),
                    BytesWritten, LossyMaxBytes);
            }
            else
            {
                UE_LOG(LogO3DWebRTCSender, Error,
                    TEXT("Payload size (%d bytes) exceeds maximum (%d bytes), consider simplifying skeleton or reducing data"),
                    BytesWritten, ReliableMaxBytes);

                {
                    FScopeLock Lock(&StatsMutex);
                    Stats.DroppedFrames++;
                }
                return false;
            }
        }
    }
    else
    {
        if (BytesWritten > ReliableMaxBytes)
        {
            UE_LOG(LogO3DWebRTCSender, Error,
                TEXT("Payload size (%d bytes) exceeds maximum (%d bytes), consider simplifying skeleton or reducing data"),
                BytesWritten, ReliableMaxBytes);

            {
                FScopeLock Lock(&StatsMutex);
                Stats.DroppedFrames++;
            }
            return false;
        }
    }

    // Send via LiveKit FFI
    LkResult Result = lk_send_data(
        ClientHandle,
        reinterpret_cast<const uint8*>(Buffer.data()),
        BytesWritten,
        Reliability
    );

    if (Result.code != 0)
    {
        FScopeLock Lock(&StatsMutex);
        Stats.DroppedFrames++;

        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to send data: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
        return false;
    }

    {
        FScopeLock Lock(&StatsMutex);
        Stats.FramesSent++;
        Stats.BytesSent += BytesWritten;
    }

    return true;
}

void FO3DWebRTCSender::Tick(float DeltaSeconds)
{
    // LiveKit FFI handles internal event processing
}

FO3DTransportStats FO3DWebRTCSender::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    return Stats;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DWebRTCSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
    FScopeLock Lock(&StateMutex);

    if (!bInitialized.Load())
    {
        return nullptr;
    }

    ActiveAudioConfig = AudioConfig;

    // Update audio options
    int32 BitrateKbps = FMath::Clamp(ActiveAudioConfig.BitrateKbps, 16, 128);
    LkResult Result = lk_set_audio_publish_options(ClientHandle, BitrateKbps * 1000, 0, ActiveAudioConfig.NumChannels > 1 ? 1 : 0);

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to update audio options: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    return MakeShared<FWebRTCSenderAudioSink, ESPMode::ThreadSafe>(*this);
}

bool FO3DWebRTCSender::ParseConfig(const FO3DTransportConfig& Config)
{
    RoomUrl = Config.Uri;
    if (RoomUrl.IsEmpty())
    {
        #if !WITH_DEV_AUTOMATION_TESTS
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("WebRTC URL not specified"));
        #endif
        return false;
    }

    Token = Config.Token;
    if (Token.IsEmpty())
    {
        #if !WITH_DEV_AUTOMATION_TESTS
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("WebRTC token not provided"));
        #endif  
        return false;
    }

    bPreferLossyData = WebRTCOptions::ParseBool(Config.AdvancedParams, WebRTCOptions::PreferLossyOptionKey, /*DefaultValue=*/false);

    return true;
}
