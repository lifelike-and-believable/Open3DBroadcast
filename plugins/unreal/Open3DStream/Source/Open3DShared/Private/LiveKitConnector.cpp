#include "LiveKitConnector.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"

#if O3DS_WITH_LIVEKIT
#include "livekit_ffi.h"
#endif

namespace
{
    static FString FromAnsi(const char* S)
    {
        return S ? FString(UTF8_TO_TCHAR(S)) : FString();
    }
}

struct FLiveKitConnector::FCallbacks
{
    // Data callback (bytes only)
    static void OnData(void* user, const uint8_t* bytes, size_t len)
    {
        FLiveKitConnector* Self = reinterpret_cast<FLiveKitConnector*>(user);
        if (!Self || !bytes || len == 0) return;
        TArray<uint8> Copy; Copy.Append(bytes, (int32)len);
        Self->OnData().Broadcast(Copy);
    }

    // Audio callback (PCM16 interleaved)
    static void OnAudio(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32 channels, int32 sample_rate)
    {
        FLiveKitConnector* Self = reinterpret_cast<FLiveKitConnector*>(user);
        if (!Self || !pcm_interleaved || frames_per_channel == 0 || channels <= 0 || sample_rate <= 0) return;
        const int32 TotalSamples = (int32)(frames_per_channel) * channels;
        FO3DSPcm16Frame Frame;
        Frame.FramesPerChannel = (int32)frames_per_channel;
        Frame.NumChannels = channels;
        Frame.SampleRate = sample_rate;
        Frame.Samples.SetNumUninitialized(TotalSamples);
        FMemory::Memcpy(Frame.Samples.GetData(), pcm_interleaved, (SIZE_T)TotalSamples * sizeof(int16));
        Self->OnRemoteAudioPcm()->Broadcast(Frame);
    }

    // Connection state callback
    static void OnConn(void* user, 
#if O3DS_WITH_LIVEKIT
        LkConnectionState state,
        int32 reason,
        const char* message
#else
        int /*state*/, int /*reason*/, const char* /*message*/
#endif
        )
    {
        FLiveKitConnector* Self = reinterpret_cast<FLiveKitConnector*>(user);
        if (!Self) return;
#if O3DS_WITH_LIVEKIT
        bool bNowOpen = false;
        const TCHAR* StateStr = TEXT("unknown");
        switch (state)
        {
        case LkConnConnecting: StateStr = TEXT("connecting"); break;
        case LkConnConnected: StateStr = TEXT("connected"); bNowOpen = true; break;
        case LkConnReconnecting: StateStr = TEXT("reconnecting"); break;
        case LkConnDisconnected: StateStr = TEXT("disconnected"); break;
        case LkConnFailed: StateStr = TEXT("failed"); break;
        default: break;
        }
        Self->bOpen = bNowOpen;
        const bool bIsError = (state == LkConnFailed);
        FString Msg = FromAnsi(message);
        if (!Msg.IsEmpty())
        {
            Self->OnState().Broadcast(FString::Printf(TEXT("%s: %s (reason=%d)"), StateStr, *Msg, reason), bIsError);
        }
        else
        {
            Self->OnState().Broadcast(StateStr, bIsError);
        }
#else
        Self->OnState().Broadcast(TEXT("unknown"), false);
#endif
    }
};

bool FLiveKitConnector::Start(const FO3DSWebRtcConfig& InConfig)
{
    Config = InConfig;
    bStarted = true;
    bOpen = false;

    OnState().Broadcast(TEXT("connecting"), false);

#if !O3DS_WITH_LIVEKIT
    UE_LOG(LogTemp, Warning, TEXT("LiveKitConnector: LiveKit FFI not available (O3DS_WITH_LIVEKIT=0)"));
    OnState().Broadcast(TEXT("livekit_unavailable"), true);
    return false;
#else
    // Create client
    LkClientHandle* Client = lk_client_create();
    if (!Client)
    {
        OnState().Broadcast(TEXT("create_failed"), true);
        return false;
    }
    ClientHandle = Client;

    // Configure diagnostics and audio format prior to connect
    lk_set_log_level(Client, LkLogInfo);
    if (Config.bEnableAudio)
    {
        const int32 bitrate_bps = FMath::Max(8000, Config.BitrateKbps * 1000);
        const int32 stereo = (Config.NumChannels >= 2) ? 1 : 0;
        lk_set_audio_publish_options(Client, bitrate_bps, /*enable_dtx*/1, stereo);
        lk_set_audio_output_format(Client, Config.SampleRate, Config.NumChannels);
    }

    // Set callbacks
    lk_client_set_data_callback(Client, &FCallbacks::OnData, this);
    lk_client_set_audio_callback(Client, &FCallbacks::OnAudio, this);
    lk_set_connection_callback(Client, &FCallbacks::OnConn, this);

    // Connect (async to receive state via callback)
    const FTCHARToUTF8 UrlUtf8(*Config.SignalingUrl);
    const FTCHARToUTF8 TokUtf8(*Config.Token);

    // Map EO3DSWebRtcRole -> LiveKit role
    LkRole Role = LkRoleBoth;
    if (Config.Role == EO3DSWebRtcRole::Client)
    {
        Role = LkRolePublisher; // Client publishes
    }
    else if (Config.Role == EO3DSWebRtcRole::Server)
    {
        Role = LkRoleSubscriber; // Server subscribes
    }

    const LkResult R = lk_connect_with_role_async(Client, UrlUtf8.Get(), TokUtf8.Get(), Role);
    if (R.code != 0)
    {
        FString Err = FromAnsi(R.message);
        OnState().Broadcast(FString::Printf(TEXT("connect_error: %s"), *Err), true);
        return false;
    }

    return true;
#endif
}

void FLiveKitConnector::Stop()
{
    if (!bStarted) return;
    bStarted = false;
    bOpen = false;

#if O3DS_WITH_LIVEKIT
    if (ClientHandle)
    {
        LkClientHandle* Client = reinterpret_cast<LkClientHandle*>(ClientHandle);
        // Unregister callbacks and clear user pointers before disconnecting to avoid
        // late calls into a destructed object.
        lk_client_set_data_callback(Client, nullptr, nullptr);
        lk_client_set_audio_callback(Client, nullptr, nullptr);
        lk_set_connection_callback(Client, nullptr, nullptr);

        lk_disconnect(Client);
        lk_client_destroy(Client);
        ClientHandle = nullptr;
    }
#endif

    OnState().Broadcast(TEXT("stopped"), false);
}

void FLiveKitConnector::Tick(float /*DeltaSeconds*/)
{
    // LiveKit FFI handles its own threads; nothing to pump here.
}

bool FLiveKitConnector::IsOpen() const
{
    return bOpen;
}

bool FLiveKitConnector::Send(const uint8* Data, int32 NumBytes)
{
#if !O3DS_WITH_LIVEKIT
    return false;
#else
    if (!ClientHandle || !Data || NumBytes <= 0) return false;
    LkClientHandle* Client = reinterpret_cast<LkClientHandle*>(ClientHandle);
    const LkResult R = lk_send_data(Client, (const uint8_t*)Data, (size_t)NumBytes, LkReliable);
    return (R.code == 0);
#endif
}

bool FLiveKitConnector::SendEx(const uint8* Data, int32 NumBytes, EO3DSReliability Mode)
{
#if !O3DS_WITH_LIVEKIT
    return false;
#else
    if (!ClientHandle || !Data || NumBytes <= 0) return false;
    LkClientHandle* Client = reinterpret_cast<LkClientHandle*>(ClientHandle);
    const LkReliability R = (Mode == EO3DSReliability::Lossy) ? LkLossy : LkReliable;
    const LkResult Res = lk_send_data(Client, (const uint8_t*)Data, (size_t)NumBytes, R);
    return (Res.code == 0);
#endif
}

bool FLiveKitConnector::EnableAudioSend(bool bEnable)
{
    bAudioSendEnabled = bEnable;
    return true; // no-op for LiveKit, but remember intent
}

bool FLiveKitConnector::SendAudioPcm16(const int16* Samples, int32 NumSamples, int32 SampleRate, int32 NumChannels)
{
#if !O3DS_WITH_LIVEKIT
    return false;
#else
    if (!ClientHandle || !Samples || NumSamples <= 0 || SampleRate <= 0 || NumChannels <= 0)
    {
        return false;
    }
    if (!bAudioSendEnabled)
    {
        // Allow sending even if flag not set, but keep behavior consistent
        // with other backends by requiring explicit enable.
        return false;
    }

    LkClientHandle* Client = reinterpret_cast<LkClientHandle*>(ClientHandle);
    const size_t FramesPerChannel = (size_t)(NumSamples / FMath::Max(1, NumChannels));
    const LkResult R = lk_publish_audio_pcm_i16(Client, (const int16_t*)Samples, FramesPerChannel, NumChannels, SampleRate);
    return (R.code == 0);
#endif
}
