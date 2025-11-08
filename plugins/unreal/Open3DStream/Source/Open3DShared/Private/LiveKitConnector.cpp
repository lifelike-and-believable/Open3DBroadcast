#include "LiveKitConnector.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Async/Async.h"

#if O3DS_WITH_LIVEKIT
#include "livekit_ffi.h"
#endif

namespace
{
    static FString FromAnsi(const char* S)
    {
        return S ? FString(UTF8_TO_TCHAR(S)) : FString();
    }

    // Ensure we explicitly load the LiveKit FFI from this plugin's Binaries folder
    // to avoid accidentally binding to an older or differently-built DLL on PATH.
    static void* GLiveKitFfiHandle = nullptr;
    static void EnsureLiveKitFfiLoaded()
    {
#if O3DS_WITH_LIVEKIT
        if (GLiveKitFfiHandle)
        {
            return; // already loaded
        }

        // Resolve plugin base dir
        FString PluginBaseDir;
        {
            TSharedPtr<IPlugin> PluginPtr = IPluginManager::Get().FindPlugin(TEXT("Open3DStream"));
            if (PluginPtr.IsValid())
            {
                PluginBaseDir = PluginPtr->GetBaseDir();
            }
        }

#if PLATFORM_WINDOWS
        const FString DllName = TEXT("livekit_ffi.dll");
        // Prefer the plugin Binaries folder (staged by Build.cs)
        FString Candidate = FPaths::Combine(PluginBaseDir, TEXT("Binaries"), TEXT("Win64"), DllName);
        if (!FPaths::FileExists(Candidate))
        {
            // Fallback to ThirdParty/bin layout inside the plugin (useful for local dev)
            Candidate = FPaths::Combine(PluginBaseDir, TEXT("ThirdParty"), TEXT("livekit_ffi"), TEXT("bin"), TEXT("Win64"), TEXT("Release"), DllName);
        }

        if (FPaths::FileExists(Candidate))
        {
            void* Handle = FPlatformProcess::GetDllHandle(*Candidate);
            if (Handle)
            {
                GLiveKitFfiHandle = Handle;
                UE_LOG(LogTemp, Log, TEXT("LiveKit FFI loaded: %s"), *Candidate);
            }
            else
            {
                UE_LOG(LogTemp, Warning, TEXT("Failed to load LiveKit FFI from %s"), *Candidate);
            }
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("LiveKit FFI DLL not found at expected locations under plugin. Falling back to default loader."));
        }
#else
        // On non-Windows platforms, rely on the runtime dependency staging for now.
#endif
#endif // O3DS_WITH_LIVEKIT
    }

    // Note: WebRTC data channels (SCTP) fragment messages internally. Avoid switching
    // reliability based on a guessed MTU, as doing so creates multiple channels and
    // can introduce cross-channel reordering. Keep a single, consistent reliability
    // mode for pose streaming to preserve temporal stability.

	// Toggle transport receive coalescing: when 1 (default) keep current coalescing;
	// when 0, broadcast every incoming packet immediately (may increase GT load).
	static TAutoConsoleVariable<int32> CVarLiveKitCoalesceReceive(
		TEXT("o3ds.LiveKit.CoalesceReceive"),
		0, // changed default: deliver on caller thread so receivers see off-thread arrivals
		TEXT("When 1, coalesce incoming LiveKit data to avoid game-thread backlog. When 0, deliver every packet immediately."),
		ECVF_Default);
}

struct FLiveKitConnector::FCallbacks
{
    // Data callback (bytes only)
    static void OnData(void* user, const uint8_t* bytes, size_t len)
    {
        FLiveKitConnector* Self = reinterpret_cast<FLiveKitConnector*>(user);
        if (!Self || !bytes || len == 0) return;

        // If coalescing is disabled, deliver every packet immediately.
        if (CVarLiveKitCoalesceReceive->GetInt() == 0)
        {
            TArray<uint8> Dispatch;
            Dispatch.AddUninitialized((int32)len);
            FMemory::Memcpy(Dispatch.GetData(), bytes, len);
            // Broadcast synchronously on the caller thread; receivers should marshal if needed.
            Self->OnData().Broadcast(Dispatch);
            return;
        }

        // Default behavior (coalesce to avoid GT backlog)
        {
            FScopeLock Lock(&Self->CoalesceMutex);
            Self->PendingLossyData.Reset((int32)len);
            Self->PendingLossyData.Append(bytes, (int32)len);
            if (!Self->bLossyDataDispatchScheduled.Load())
            {
                Self->bLossyDataDispatchScheduled.Store(true);
                // Schedule minimal task to flush latest frame; if game thread is busy, older frames are overwritten before execution.
                AsyncTask(ENamedThreads::GameThread, [Weak = TWeakPtr<IWebRTCConnector>(Self->AsShared())]()
                {
                    if (TSharedPtr<IWebRTCConnector> P = Weak.Pin())
                    {
                        FLiveKitConnector* Concrete = static_cast<FLiveKitConnector*>(P.Get());
                        TArray<uint8> Dispatch;
                        {
                            FScopeLock Lock2(&Concrete->CoalesceMutex);
                            Dispatch = Concrete->PendingLossyData; // copy latest
                            Concrete->bLossyDataDispatchScheduled.Store(false);
                        }
                        if (Dispatch.Num() > 0)
                        {
                            Concrete->OnData().Broadcast(Dispatch);
                        }
                    }
                });
            }
        }
    }

    // Audio callback (PCM16 interleaved)
    static void OnAudio(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32 channels, int32 sample_rate)
    {
        FLiveKitConnector* Self = reinterpret_cast<FLiveKitConnector*>(user);
        if (!Self || !pcm_interleaved || frames_per_channel == 0 || channels <= 0 || sample_rate <= 0) return;
        // Do not coalesce or drop audio frames; broadcast every PCM buffer.
        FO3DSPcm16Frame Frame;
        Frame.FramesPerChannel = (int32)frames_per_channel;
        Frame.NumChannels = channels;
        Frame.SampleRate = sample_rate;
        const int32 TotalSamples = (int32)(frames_per_channel) * channels;
        Frame.Samples.SetNumUninitialized(TotalSamples);
        FMemory::Memcpy(Frame.Samples.GetData(), pcm_interleaved, (SIZE_T)TotalSamples * sizeof(int16));
        if (FO3DSOnWebRtcPcm16* D = Self->OnRemoteAudioPcm())
        {
            D->Broadcast(Frame);
        }
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
        case LkConnConnected: StateStr = TEXT("DataChannelOpen"); bNowOpen = true; break;
        case LkConnReconnecting: StateStr = TEXT("reconnecting"); break;
        case LkConnDisconnected: StateStr = TEXT("DataChannelClosed"); break;
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
    // Proactively load our FFI DLL from the plugin to guarantee the correct build (with TLS) is used.
    EnsureLiveKitFfiLoaded();

    Config = InConfig;
    bStarted = true;
    bOpen = false;
    // Reset pacing state on (re)start
    LastDataSendSec = 0.0;
    NextPacedSendSec = 0.0;
    bPacingPrimed = false;
    {
        FScopeLock L(&OutPaceMutex);
        PendingLossyOut.Reset();
        bHasPendingLossyOut.Store(false);
    }

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
        // Disable DTX (discontinuous transmission) to avoid pops at talk/silence boundaries for continuous streams.
        lk_set_audio_publish_options(Client, bitrate_bps, /*enable_dtx*/0, stereo);
        lk_set_audio_output_format(Client, Config.SampleRate, Config.NumChannels);
    }

    // Set callbacks
    lk_client_set_data_callback(Client, &FCallbacks::OnData, this);
    lk_client_set_audio_callback(Client, &FCallbacks::OnAudio, this);
    lk_set_connection_callback(Client, &FCallbacks::OnConn, this);

    // Provide distinct labels for reliable vs lossy channels (optional)
    lk_set_default_data_labels(Client, "o3ds-rel", "o3ds-lossy");

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
    // Periodic diagnostics (non-blocking) when verbose
#if O3DS_WITH_LIVEKIT
    // Outgoing pacing: forward-scheduled cadence reduces jitter vs. delta-only timing.
    if (ClientHandle && bOpen && Config.bEnableSendPacing && Config.bPreferLossyData)
    {
        const double Now = FPlatformTime::Seconds();
        const double Interval = 1.0 / FMath::Max(1, Config.TargetDataSendHz);
        if (!bPacingPrimed)
        {
            NextPacedSendSec = Now + Interval; // start after one interval to allow initial coalescing
            bPacingPrimed = true;
        }
        // Allow slight catch-up if we overshoot, but never send more than once per Tick even if multiple intervals skipped.
        if (bHasPendingLossyOut.Load() && Now >= NextPacedSendSec)
        {
            TArray<uint8> Payload;
            {
                FScopeLock Lock(&OutPaceMutex);
                Payload = PendingLossyOut; // copy latest
                bHasPendingLossyOut.Store(false);
            }
            if (Payload.Num() > 0)
            {
                LkClientHandle* Client = reinterpret_cast<LkClientHandle*>(ClientHandle);
                const LkReliability Rel = LkLossy;
                // Use unordered for lossy so receiver doesn't stall waiting on missing sequence numbers.
                const int32 Ordered = 0;
                const char* Label = "o3ds-lossy";
                lk_send_data_ex(Client, Payload.GetData(), (size_t)Payload.Num(), Rel, Ordered, Label);
            }
            // Advance schedule by fixed steps until it's in the future to avoid drift accumulation.
            do { NextPacedSendSec += Interval; } while (NextPacedSendSec <= Now);
        }
    }
    if (ClientHandle && Config.bVerbose)
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - LastStatsLogSec > 5.0) // every ~5s
        {
            LastStatsLogSec = Now;
            LkDataStats DataStats{};
            if (lk_get_data_stats(reinterpret_cast<LkClientHandle*>(ClientHandle), &DataStats).code == 0)
            {
                FString Msg = FString::Printf(TEXT("data_stats rel_bytes=%lld rel_drop=%lld lossy_bytes=%lld lossy_drop=%lld"),
                    (long long)DataStats.reliable_sent_bytes,
                    (long long)DataStats.reliable_dropped,
                    (long long)DataStats.lossy_sent_bytes,
                    (long long)DataStats.lossy_dropped);
                OnState().Broadcast(Msg, false);
            }
            LkAudioStats AudioStats{};
            if (lk_get_audio_stats(reinterpret_cast<LkClientHandle*>(ClientHandle), &AudioStats).code == 0)
            {
                FString Msg = FString::Printf(TEXT("audio_stats sr=%d ch=%d queued=%d/%d underrun=%d overrun=%d"),
                    AudioStats.sample_rate,
                    AudioStats.channels,
                    AudioStats.ring_queued_frames,
                    AudioStats.ring_capacity_frames,
                    AudioStats.underruns,
                    AudioStats.overruns);
                OnState().Broadcast(Msg, false);
            }
        }
    }
#endif
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
    // Avoid building up a backlog inside the transport while connecting.
    if (Config.bRequireOpenBeforeSend && !bOpen)
    {
        return false;
    }
    if (Config.bEnableSendPacing && Config.bPreferLossyData)
    {
        FScopeLock Lock(&OutPaceMutex);
        PendingLossyOut.Reset(NumBytes);
        PendingLossyOut.Append(Data, NumBytes);
        bHasPendingLossyOut.Store(true);
        return true; // queued for paced send
    }
    LkClientHandle* Client = reinterpret_cast<LkClientHandle*>(ClientHandle);
    LkReliability Rel = Config.bPreferLossyData ? LkLossy : LkReliable;
    // For lossy delivery use unordered to avoid stall on dropped frames; keep ordered for reliable.
    const int32 Ordered = (Rel == LkReliable) ? 1 : 0;
    const char* Label = (Rel == LkReliable) ? "o3ds-rel" : "o3ds-lossy";
    const LkResult R = lk_send_data_ex(Client, (const uint8_t*)Data, (size_t)NumBytes, Rel, Ordered, Label);
    return (R.code == 0);
#endif
}

bool FLiveKitConnector::SendEx(const uint8* Data, int32 NumBytes, EO3DSReliability Mode)
{
#if !O3DS_WITH_LIVEKIT
    return false;
#else
    if (!ClientHandle || !Data || NumBytes <= 0) return false;
    if (Config.bRequireOpenBeforeSend && !bOpen)
    {
        return false;
    }
    LkReliability Rel = (Mode == EO3DSReliability::Lossy) ? LkLossy : LkReliable;
    if (Rel == LkLossy && Config.bEnableSendPacing)
    {
        FScopeLock Lock(&OutPaceMutex);
        PendingLossyOut.Reset(NumBytes);
        PendingLossyOut.Append(Data, NumBytes);
        bHasPendingLossyOut.Store(true);
        return true; // queued
    }
    LkClientHandle* Client = reinterpret_cast<LkClientHandle*>(ClientHandle);
    const int32 Ordered = (Rel == LkReliable) ? 1 : 0; // unordered when lossy
    const char* Label = (Rel == LkLossy) ? "o3ds-lossy" : "o3ds-rel";
    const LkResult Res = lk_send_data_ex(Client, (const uint8_t*)Data, (size_t)NumBytes, Rel, Ordered, Label);
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
    if (!bOpen)
    {
        // Prevent pre-connection accumulation.
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
