#include "WebRTCReceiver.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "livekit_ffi.h"
#include "o3ds/model.h"

namespace
{
    static FString FromAnsi(const char* S)
    {
        return S ? FString(UTF8_TO_TCHAR(S)) : FString();
    }
}

// Static callback for connection state changes
void FO3DWebRTCReceiver::OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message)
{
    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);
    if (!Self) return;

    switch (state)
    {
    case LkConnConnecting:
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connecting..."));
        break;

    case LkConnConnected:
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connected"));
        Self->bConnected.Store(true);
        break;

    case LkConnReconnecting:
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver reconnecting..."));
        Self->bConnected.Store(false);
        break;

    case LkConnDisconnected:
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver disconnected: %s"), *FromAnsi(message));
        Self->bConnected.Store(false);
        break;

    case LkConnFailed:
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver connection failed (code=%d): %s"), reason_code, *FromAnsi(message));
        Self->bConnected.Store(false);
        break;
    }
}

// Static callback for incoming data
void FO3DWebRTCReceiver::OnDataReceived(void* user, const uint8_t* bytes, size_t len)
{
    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);
    if (!Self || !bytes || len == 0) return;

    const double NowSeconds = FPlatformTime::Seconds();

    // Update stats
    {
        FScopeLock Lock(&Self->StatsMutex);
        Self->Stats.FramesReceived++;
        Self->Stats.BytesReceived += len;

        // TODO: Extract timestamp from payload for latency calculation
        // For now, assume minimal latency since LiveKit handles this internally
        Self->AccumulateLatency(10.0); // Placeholder: 10ms assumed latency
    }

    // Forward to consumer
    if (Self->Consumer.IsValid())
    {
        TArray<uint8> Payload;
        Payload.AddUninitialized((int32)len);
        FMemory::Memcpy(Payload.GetData(), bytes, len);

        Self->Consumer->SubmitFrame(Self->SubjectName, Payload, NowSeconds);
    }
}

// Static callback for incoming audio
void FO3DWebRTCReceiver::OnAudioReceived(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate)
{
    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);
    if (!Self || !pcm_interleaved || frames_per_channel == 0 || channels <= 0 || sample_rate <= 0) return;

    {
        FScopeLock Lock(&Self->StatsMutex);
        Self->Stats.BytesReceived += frames_per_channel * channels * sizeof(int16);
    }

    if (Self->AudioSink.IsValid())
    {
        // Fill audio metadata
        O3DS::FAudioFrameMeta Meta;
        Meta.SampleRate = sample_rate;
        Meta.NumChannels = channels;
        Meta.StreamLabel = Self->SubjectName;

        const size_t TotalSamples = frames_per_channel * channels;
        const size_t NumBytes = TotalSamples * sizeof(int16);

        Self->AudioSink->SubmitPcm16(Meta, reinterpret_cast<const uint8*>(pcm_interleaved), (int32)NumBytes);
    }
    else
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - Self->LastAudioDropLogTime.Load() > 1.0)
        {
            UE_LOG(LogO3DWebRTCReceiver, Verbose, TEXT("WebRTC audio frame discarded (no sink) - frames=%d channels=%d sr=%d"),
                (int32)frames_per_channel, channels, sample_rate);
            Self->LastAudioDropLogTime.Store(Now);
        }
    }
}

FO3DWebRTCReceiver::FO3DWebRTCReceiver()
{
}

FO3DWebRTCReceiver::~FO3DWebRTCReceiver()
{
    Stop();
}

bool FO3DWebRTCReceiver::Initialize(const FO3DTransportConfig& Config)
{
    FScopeLock Lock(&StateMutex);

    if (bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver already initialized"));
        return false;
    }

    if (!ParseConfig(Config))
    {
        return false;
    }

    // Create LiveKit client handle
    ClientHandle = lk_client_create();
    if (!ClientHandle)
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to create LiveKit client"));
        return false;
    }

    // Set connection callback
    LkResult Result = lk_set_connection_callback(ClientHandle, FO3DWebRTCReceiver::OnConnectionState, this);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Failed to set connection callback: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    // Set data callback
    Result = lk_client_set_data_callback(ClientHandle, FO3DWebRTCReceiver::OnDataReceived, this);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Failed to set data callback: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    // Set audio callback
    Result = lk_client_set_audio_callback(ClientHandle, FO3DWebRTCReceiver::OnAudioReceived, this);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Failed to set audio callback: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    // Configure audio output format if needed
    if (Config.Audio.bEnableAudio)
    {
        ActiveAudioConfig = Config.Audio;
        Result = lk_set_audio_output_format(ClientHandle, ActiveAudioConfig.SampleRate, ActiveAudioConfig.NumChannels);

        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Failed to set audio output format: %s"), *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
        }
    }

    ActiveConfig = Config;
    Stats.Reset();
    LatencySamples = 0;
    LastAudioDropLogTime = 0.0;
    bInitialized.Store(true);

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver initialized: URL=%s"),
        *RoomUrl);

    return true;
}

void FO3DWebRTCReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
    FScopeLock Lock(&StateMutex);
    Consumer = InConsumer;
}

bool FO3DWebRTCReceiver::Start()
{
    FScopeLock Lock(&StateMutex);

    if (!bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Cannot start WebRTC receiver: not initialized"));
        return false;
    }

    if (bConnected.Load())
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver already connected"));
        return true;
    }

    // Create default consumer if not set
    if (!Consumer.IsValid())
    {
        Consumer = FSerializedFrameConsumerRegistry::Create();
        if (!Consumer.IsValid())
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("No serialized frame consumer registered; frames will be dropped"));
        }
    }

    // Connect asynchronously with Subscriber role
    LkResult Result = lk_connect_with_role_async(
        ClientHandle,
        TCHAR_TO_UTF8(*RoomUrl),
        TCHAR_TO_UTF8(*Token),
        LkRoleSubscriber
    );

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to connect: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
        return false;
    }

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connecting..."));
    return true;
}

void FO3DWebRTCReceiver::Stop()
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
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Disconnect warning: %s"), *FromAnsi(Result.message));
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    lk_client_destroy(ClientHandle);
    ClientHandle = nullptr;

    Consumer.Reset();
    AudioSink.Reset();

    bConnected.Store(false);
    bInitialized.Store(false);

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver stopped"));
}

int32 FO3DWebRTCReceiver::Poll()
{
    // LiveKit FFI delivers data via callbacks, so Poll() is a no-op
    // Callbacks handle data delivery directly
    return 0;
}

FO3DTransportStats FO3DWebRTCReceiver::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    return Stats;
}

void FO3DWebRTCReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
    FScopeLock Lock(&StateMutex);
    AudioSink = Sink;
    ActiveAudioConfig = AudioConfig;

    if (ClientHandle && bInitialized.Load())
    {
        LkResult Result = lk_set_audio_output_format(ClientHandle, ActiveAudioConfig.SampleRate, ActiveAudioConfig.NumChannels);
        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Failed to update audio output format: %s"), *FromAnsi(Result.message));
            if (Result.message)
            {
                lk_free_str(const_cast<char*>(Result.message));
            }
        }
    }
}

bool FO3DWebRTCReceiver::ParseConfig(const FO3DTransportConfig& Config)
{
    RoomUrl = Config.Uri;
    if (RoomUrl.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC URL not specified"));
        return false;
    }

    Token = Config.Token;
    if (Token.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC token not provided"));
        return false;
    }

    // Use StreamId as subject name if provided, otherwise use a default
    SubjectName = Config.StreamId.IsEmpty() ? TEXT("WebRTCStream") : Config.StreamId;

    return true;
}

void FO3DWebRTCReceiver::AccumulateLatency(double LatencyMs)
{
    FScopeLock Lock(&StatsMutex);

    Stats.MaxLatencyMs = FMath::Max(Stats.MaxLatencyMs, LatencyMs);

    const int64 NewSampleCount = LatencySamples + 1;
    const double PreviousTotal = Stats.AverageLatencyMs * LatencySamples;
    Stats.AverageLatencyMs = (PreviousTotal + LatencyMs) / FMath::Max<int64>(1, NewSampleCount);
    LatencySamples = NewSampleCount;
}
