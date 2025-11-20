#include "WebRTCReceiver.h"
#include "../Shared/WebRTCUtils.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Logging/LogMacros.h"
#include "Misc/Paths.h"
#include "livekit_ffi.h"
#include "o3ds/model.h"

using WebRTCUtils::FromAnsi;

namespace
{
    static constexpr TCHAR ReconnectTimeoutOptionKey[] = TEXT("webrtc.reconnect_timeout");

    FString GetPluginBaseDir()
    {
        if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Open3DBroadcast")))
        {
            return Plugin->GetBaseDir();
        }
        return FString();
    }

    static void* GLiveKitFfiHandle = nullptr;

    void EnsureLiveKitFfiLoaded()
    {
#if PLATFORM_WINDOWS
        if (GLiveKitFfiHandle)
        {
            return;
        }

        const FString PluginBaseDir = GetPluginBaseDir();
        if (PluginBaseDir.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Unable to locate Open3DBroadcast plugin directory while loading LiveKit FFI."));
        }

        const FString DllName = TEXT("livekit_ffi.dll");
        FString CandidatePath;
        if (!PluginBaseDir.IsEmpty())
        {
            CandidatePath = FPaths::Combine(PluginBaseDir, TEXT("Binaries"), TEXT("Win64"), DllName);
            if (!FPaths::FileExists(CandidatePath))
            {
                CandidatePath = FPaths::Combine(PluginBaseDir, TEXT("ThirdParty"), TEXT("livekit_ffi"), TEXT("bin"), TEXT("Win64"), TEXT("Release"), DllName);
            }
        }

        if (!CandidatePath.IsEmpty() && FPaths::FileExists(CandidatePath))
        {
            void* Handle = FPlatformProcess::GetDllHandle(*CandidatePath);
            if (Handle)
            {
                GLiveKitFfiHandle = Handle;
                UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("LiveKit FFI loaded from %s"), *CandidatePath);
            }
            else
            {
                UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Failed to load LiveKit FFI from %s"), *CandidatePath);
            }
        }
        else
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("LiveKit FFI DLL not found near plugin; relying on system loader."));
        }
#else
        // Non-Windows platforms rely on the runtime dependency staging provided by the build scripts.
#endif
    }

    void LogIfFailed(const LkResult& Result, const TCHAR* Context)
    {
        if (Result.code == 0)
        {
            return;
        }

        const FString Message = FromAnsi(Result.message);
        if (Message.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("%s failed (code=%d)"), Context, Result.code);
        }
        else
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("%s failed (code=%d): %s"), Context, Result.code, *Message);
        }

        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
    }

    double ParseDoubleOption(const TMap<FString, FString>& Params, const TCHAR* Key, double DefaultValue)
    {
        if (!Key)
        {
            return DefaultValue;
        }

        if (const FString* Value = Params.Find(Key))
        {
            if (!Value->IsEmpty())
            {
                const double Parsed = FCString::Atod(**Value);
                if (Parsed == 0.0 || FMath::IsFinite(Parsed))
                {
                    return Parsed;
                }
            }
        }
        return DefaultValue;
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
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] OnConnectionState: LkConnConnecting"));
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connecting..."));
        break;

    case LkConnConnected:
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] OnConnectionState: LkConnConnected - data/audio callbacks should now fire"));
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connected"));
        Self->bConnected.Store(true);
        Self->bPendingAudioFormatApply.Store(true);
        Self->bReconnectPending.Store(false);
        {
            FScopeLock DataLock(&Self->LastDataMutex);
            Self->LastDataReceiveTime = FPlatformTime::Seconds();
        }
        break;

    case LkConnReconnecting:
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] OnConnectionState: LkConnReconnecting"));
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver reconnecting..."));
        Self->bConnected.Store(false);
        break;

    case LkConnDisconnected:
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] OnConnectionState: LkConnDisconnected - %hs"), message);
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver disconnected: %s"), *FromAnsi(message));
        Self->bConnected.Store(false);
        Self->RequestReconnect(true);
        break;

    case LkConnFailed:
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] OnConnectionState: LkConnFailed (code=%d) - %hs"), reason_code, message);
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver connection failed (code=%d): %s"), reason_code, *FromAnsi(message));
        Self->bConnected.Store(false);
        Self->RequestReconnect(true);
        break;
    }
}

// Static callback for incoming data with label and reliability info
void FO3DWebRTCReceiver::OnDataReceivedEx(void* user, const char* label, LkReliability reliability, const uint8_t* bytes, size_t len)
{
    // DIAGNOSTIC: Trace entry point to confirm callback is being invoked
    static TAtomic<uint64> CallCount{ 0 };
    uint64 ThisCallNumber = ++CallCount;

    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);

    // Log EVERY invocation, even with null/zero parameters (Verbose to avoid spam)
    UE_LOG(LogO3DWebRTCReceiver, Verbose,
        TEXT("[DIAG] OnDataReceivedEx INVOKED (call#%llu): label='%hs' len=%zu user=%p"),
        ThisCallNumber, label ? label : "(NULL)", len, user);

    if (!Self || !bytes || len == 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("[DIAG] OnDataReceivedEx early-return: Self=%p bytes=%p len=%zu"),
            Self, bytes, len);
        return;
    }

    // ARCHITECTURE VERIFICATION: Log data reception with label (Verbose to avoid spam)
    UE_LOG(LogO3DWebRTCReceiver, Verbose,
        TEXT("[ARCH] OnDataReceivedEx ENTRY (call#%llu): label='%hs' len=%zu reliability=%s"),
        ThisCallNumber, label ? label : "(NULL)", len,
        reliability == LkReliable ? TEXT("Reliable") : TEXT("Lossy"));

    // Convert label to FString with caching (optimization: avoids repeated UTF8→UTF16 conversion)
    FString SubjectLabel = Self->GetOrCacheSubjectLabel(label);

    FO3DWebRTCReceiver::FPendingFrame Frame;
    Frame.EnqueueTimeSeconds = FPlatformTime::Seconds();
    Frame.ReserveForTypicalFrame();  // Pre-allocate to typical frame size (optimization)
    Frame.Payload.AddUninitialized((int32)len);
    FMemory::Memcpy(Frame.Payload.GetData(), bytes, len);

    {
        FScopeLock Lock(&Self->PendingFramesMutex);
        TArray<FPendingFrame>& SubjectQueue = Self->PendingFramesBySubject.FindOrAdd(SubjectLabel);
        SubjectQueue.Emplace(MoveTemp(Frame));

        // ARCHITECTURE VERIFICATION: Log queue status after enqueue (Verbose to avoid spam)
        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("[ARCH] OnDataReceivedEx ENQUEUED: label='%s' %d bytes (QueueLen=%d)"),
            *SubjectLabel, static_cast<int32>(len), SubjectQueue.Num());

        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("OnDataReceivedEx label='%s' %d bytes (Queue=%d, Reliability=%s)"),
            *SubjectLabel, static_cast<int32>(len), SubjectQueue.Num(),
            reliability == LkReliable ? TEXT("Reliable") : TEXT("Lossy"));
    }

    {
        FScopeLock DataLock(&Self->LastDataMutex);
        Self->LastDataReceiveTime = FPlatformTime::Seconds();
    }

    Self->bReconnectPending.Store(false);
}

// FALLBACK: Unlabeled data callback
// This is registered if labeled channels don't work.
// All data comes in via default/unnamed channel with no subject label.
void FO3DWebRTCReceiver::OnDataReceived(void* user, const uint8_t* bytes, size_t len)
{
    static TAtomic<uint64> CallCount{ 0 };
    uint64 ThisCallNumber = ++CallCount;

    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);

    // Log EVERY invocation
    UE_LOG(LogO3DWebRTCReceiver, Verbose,
        TEXT("[DIAG] OnDataReceived INVOKED (call#%llu): len=%zu user=%p (FALLBACK - UNLABELED CHANNEL)"),
        ThisCallNumber, len, user);

    if (!Self || !bytes || len == 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("[DIAG] OnDataReceived early-return: Self=%p bytes=%p len=%zu"),
            Self, bytes, len);
        return;
    }

    // ARCHITECTURE VERIFICATION: Log data reception (no label, so use default)
    UE_LOG(LogO3DWebRTCReceiver, Verbose,
        TEXT("[ARCH] OnDataReceived ENTRY (call#%llu): len=%zu (received on DEFAULT/UNNAMED channel)"),
        ThisCallNumber, len);

    // Use default label since this is unlabeled channel
    FString SubjectLabel = TEXT("default");

    FO3DWebRTCReceiver::FPendingFrame Frame;
    Frame.EnqueueTimeSeconds = FPlatformTime::Seconds();
    Frame.ReserveForTypicalFrame();  // Pre-allocate to typical frame size (optimization)
    Frame.Payload.AddUninitialized((int32)len);
    FMemory::Memcpy(Frame.Payload.GetData(), bytes, len);

    {
        FScopeLock Lock(&Self->PendingFramesMutex);
        TArray<FPendingFrame>& SubjectQueue = Self->PendingFramesBySubject.FindOrAdd(SubjectLabel);
        SubjectQueue.Emplace(MoveTemp(Frame));

        // ARCHITECTURE VERIFICATION: Log queue status
        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("[ARCH] OnDataReceived ENQUEUED: label='%s' %d bytes (QueueLen=%d)"),
            *SubjectLabel, static_cast<int32>(len), SubjectQueue.Num());

        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("OnDataReceived %d bytes queued as '%s' (Queue=%d)"),
            static_cast<int32>(len), *SubjectLabel, SubjectQueue.Num());
    }

    {
        FScopeLock DataLock(&Self->LastDataMutex);
        Self->LastDataReceiveTime = FPlatformTime::Seconds();
    }

    Self->bReconnectPending.Store(false);
}

// Static callback for incoming audio
// NOTE: The current LiveKit FFI does not provide per-audio-track label information in the audio callback.
// All audio is received in a single stream. When LiveKit FFI is updated to support
// LkAudioCallbackEx with label information, audio can be routed per-subject like data channels.
// For now, audio is delivered to the audio sink with StreamLabel set to a generic identifier.
// New callback with per-subject audio identification (participant_name and track_name from LiveKit FFI)
void FO3DWebRTCReceiver::OnAudioReceivedEx(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate, const char* participant_name, const char* track_name)
{
    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);
    if (!Self || !pcm_interleaved || frames_per_channel == 0 || channels <= 0 || sample_rate <= 0) return;

    // Lock-free atomic updates for high-frequency stats (optimization: replace mutex with atomics)
    Self->AtomicFramesReceived.IncrementExchange();
    Self->AtomicBytesReceived.AddExchange(static_cast<int64>(frames_per_channel * channels) * static_cast<int64>(sizeof(int16)));

    if (Self->AudioSink.IsValid())
    {
        // IMPORTANT: Audio labeling strategy
        // - track_name: The subject identifier we set during track creation (e.g., "Quincy")
        // - participant_name: The sender/participant publishing this audio
        //
        // LiveKit now correctly preserves the track_name we set during track creation.
        // We use track_name to identify which subject's audio this is.
        // This allows audio to be routed to the same subject as mocap data (which uses the same label).
        FString SubjectLabel = track_name && *track_name
            ? Self->GetOrCacheSubjectLabel(track_name)
            : TEXT("audio_default");

        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("[DIAG] OnAudioReceivedEx: track='%hs' (subject='%s') from participant='%hs' frames=%zu channels=%d sample_rate=%d"),
            track_name ? track_name : "(unknown)",
            *SubjectLabel,
            participant_name ? participant_name : "(unknown)",
            frames_per_channel, channels, sample_rate);

        // Fill audio metadata with track name as subject label (per-subject routing)
        O3DS::FAudioFrameMeta Meta;
        Meta.SampleRate = sample_rate;
        Meta.NumChannels = channels;
        Meta.StreamLabel = SubjectLabel;  // Track name identifies the subject

        const size_t TotalSamples = frames_per_channel * channels;
        const size_t NumBytes = TotalSamples * sizeof(int16);

        Self->AudioSink->SubmitPcm16(Meta, reinterpret_cast<const uint8*>(pcm_interleaved), (int32)NumBytes);
    }
    else
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - Self->LastAudioDropLogTime > 1.0)
        {
            UE_LOG(LogO3DWebRTCReceiver, Verbose, TEXT("WebRTC audio frame discarded (no sink) - participant='%hs' track='%hs' frames=%d channels=%d sr=%d"),
                participant_name ? participant_name : "(unknown)", track_name ? track_name : "(unknown)", (int32)frames_per_channel, channels, sample_rate);
            Self->LastAudioDropLogTime = Now;
        }
    }
}

// Legacy callback (kept for backwards compatibility, but OnAudioReceivedEx should be used)
void FO3DWebRTCReceiver::OnAudioReceived(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate)
{
    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);
    if (!Self || !pcm_interleaved || frames_per_channel == 0 || channels <= 0 || sample_rate <= 0) return;

    // Lock-free atomic updates for high-frequency stats (optimization: replace mutex with atomics)
    Self->AtomicFramesReceived.IncrementExchange();
    Self->AtomicBytesReceived.AddExchange(static_cast<int64>(frames_per_channel * channels) * static_cast<int64>(sizeof(int16)));

    if (Self->AudioSink.IsValid())
    {
        // Fill audio metadata
        O3DS::FAudioFrameMeta Meta;
        Meta.SampleRate = sample_rate;
        Meta.NumChannels = channels;
        // Use a generic stream label since audio label routing is not yet available in LiveKit FFI
        // This will be updated when lk_client_set_audio_callback_ex() becomes available
        Meta.StreamLabel = TEXT("audio_default");

        const size_t TotalSamples = frames_per_channel * channels;
        const size_t NumBytes = TotalSamples * sizeof(int16);

        Self->AudioSink->SubmitPcm16(Meta, reinterpret_cast<const uint8*>(pcm_interleaved), (int32)NumBytes);
    }
    else
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - Self->LastAudioDropLogTime > 1.0)
        {
            UE_LOG(LogO3DWebRTCReceiver, Verbose, TEXT("WebRTC audio frame discarded (no sink) - frames=%d channels=%d sr=%d"),
                (int32)frames_per_channel, channels, sample_rate);
            Self->LastAudioDropLogTime = Now;
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

    // Platform validation: WebRTC module currently supports Windows 64-bit only
#if !PLATFORM_WINDOWS || !PLATFORM_64BITS
    UE_LOG(LogO3DWebRTCReceiver, Error,
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

    bConnected.Store(false);

    UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] Initialize() called - starting setup"));

    if (!ParseConfig(Config))
    {
        return false;
    }

    EnsureLiveKitFfiLoaded();

    ActiveAudioConfig = Config.Audio;

    UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] About to call SetupClientHandle()"));
    if (!SetupClientHandle())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("[DIAG] SetupClientHandle() FAILED"));
        return false;
    }

    UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] SetupClientHandle() SUCCESS - callbacks should be registered"));
    ActiveConfig = Config;
    Stats.Reset();
    LatencySamples = 0;
    LastAudioDropLogTime = 0.0;
    {
        FScopeLock DataLock(&LastDataMutex);
        LastDataReceiveTime = FPlatformTime::Seconds();
    }
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

    if (!SetupClientHandle())
    {
        return false;
    }

    if (!BeginConnect())
    {
        return false;
    }

    {
        FScopeLock DataLock(&LastDataMutex);
        LastDataReceiveTime = FPlatformTime::Seconds();
    }

    bReconnectPending.Store(false);
    return true;
}

void FO3DWebRTCReceiver::Stop()
{
    FScopeLock Lock(&StateMutex);

    if (ClientHandle)
    {
        lk_client_set_data_callback_ex(ClientHandle, nullptr, nullptr);
        lk_client_set_audio_callback(ClientHandle, nullptr, nullptr);
        lk_set_connection_callback(ClientHandle, nullptr, nullptr);

        LogIfFailed(lk_disconnect(ClientHandle), TEXT("LiveKit disconnect"));

        lk_client_destroy(ClientHandle);
        ClientHandle = nullptr;
    }

    Consumer.Reset();
    AudioSink.Reset();
    {
        FScopeLock PendingLock(&PendingFramesMutex);
        PendingFramesBySubject.Reset();
    }

    bConnected.Store(false);
    bInitialized.Store(false);
    bPendingAudioFormatApply.Store(false);
    bReconnectPending.Store(false);

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver stopped"));
}

int32 FO3DWebRTCReceiver::Poll()
{
    // Process frames from each subject's queue with batch delivery
    // Instead of keeping only the latest frame (which causes frame drops),
    // we now process all queued frames to improve smoothness and reduce latency
    TMap<FString, TArray<FPendingFrame>> AllFramesBySubject;

    {
        FScopeLock Lock(&PendingFramesMutex);

        // ARCHITECTURE VERIFICATION: Log queue state at start of Poll (Verbose - fires every frame)
        UE_LOG(LogO3DWebRTCReceiver, Verbose,
            TEXT("[ARCH] Poll() START: %d subjects have pending frames"),
            PendingFramesBySubject.Num());

        int32 TotalQueuedFrames = 0;
        for (auto& SubjectQueue : PendingFramesBySubject)
        {
            const FString& SubjectLabel = SubjectQueue.Key;
            TArray<FPendingFrame>& Frames = SubjectQueue.Value;

            if (Frames.Num() > 0)
            {
                // OPTIMIZATION: Process ALL queued frames, not just the latest
                // This improves frame delivery consistency and reduces artificial latency
                AllFramesBySubject.Add(SubjectLabel, MoveTemp(Frames));
                TotalQueuedFrames += AllFramesBySubject[SubjectLabel].Num();

                // ARCHITECTURE VERIFICATION: Log frame extraction per subject (Verbose)
                UE_LOG(LogO3DWebRTCReceiver, Verbose,
                    TEXT("[ARCH] Poll() DEQUEUED: subject='%s' frames=%d total_bytes=%d"),
                    *SubjectLabel, AllFramesBySubject[SubjectLabel].Num(),
                    AllFramesBySubject[SubjectLabel].Num() > 0 ? AllFramesBySubject[SubjectLabel].Last().Payload.Num() : 0);
            }
        }
    }

    int32 FramesProcessed = 0;
    const double NowSeconds = FPlatformTime::Seconds();

    // ARCHITECTURE VERIFICATION: Log consumer submission phase (Verbose)
    UE_LOG(LogO3DWebRTCReceiver, Verbose,
        TEXT("[ARCH] Poll() SUBMIT: processing frames from %d subjects"),
        AllFramesBySubject.Num());

    // Submit all queued frames from each subject to the consumer
    if (Consumer.IsValid())
    {
        for (auto& SubjectEntry : AllFramesBySubject)
        {
            const FString& SubjectLabel = SubjectEntry.Key;
            TArray<FPendingFrame>& Frames = SubjectEntry.Value;

            for (FPendingFrame& Frame : Frames)
            {
                const double ReceiveLatencyMs = FMath::Max(0.0, (NowSeconds - Frame.EnqueueTimeSeconds) * 1000.0);

                // Update frame and byte stats with lock-free atomics (optimization)
                AtomicFramesReceived.IncrementExchange();
                AtomicBytesReceived.AddExchange(Frame.Payload.Num());

                // Latency tracking still uses mutex (calculated less frequently)
                {
                    FScopeLock Lock(&StatsMutex);
                    Stats.MaxLatencyMs = FMath::Max(Stats.MaxLatencyMs, ReceiveLatencyMs);
                    const int64 NewSampleCount = LatencySamples + 1;
                    const double PreviousTotal = Stats.AverageLatencyMs * LatencySamples;
                    Stats.AverageLatencyMs = (PreviousTotal + ReceiveLatencyMs) / FMath::Max<int64>(1, NewSampleCount);
                    LatencySamples = NewSampleCount;
                }

                // ARCHITECTURE VERIFICATION: Log frame submission (Verbose)
                UE_LOG(LogO3DWebRTCReceiver, Verbose,
                    TEXT("[ARCH] Poll() SUBMITTING: subject='%s' bytes=%d to consumer"),
                    *SubjectLabel, Frame.Payload.Num());

                // PHASE 13: Timestamp alignment fix
                // LiveLink expects WorldTime to be "when to display" not "when it arrived"
                // Using FPlatformTime::Seconds() (current time) instead of Frame.EnqueueTimeSeconds (arrival time)
                // This prevents LiveLink from buffering frames as "old" data
                const double SubmitTimeNow = FPlatformTime::Seconds();

                // Submit frame to consumer with the subject label as the subject name
                // Using current time (SubmitTimeNow) instead of old arrival time
                Consumer->SubmitFrame(SubjectLabel, Frame.Payload, SubmitTimeNow);
                FramesProcessed++;

                // ARCHITECTURE VERIFICATION: Log successful submission (Verbose)
                UE_LOG(LogO3DWebRTCReceiver, Verbose,
                    TEXT("[ARCH] Poll() SUBMITTED: subject='%s' (FramesProcessed=%d)"),
                    *SubjectLabel, FramesProcessed);
            }
        }
    }
    else
    {
        // ARCHITECTURE VERIFICATION: Log no consumer case (Warning - error condition)
        UE_LOG(LogO3DWebRTCReceiver, Warning,
            TEXT("[ARCH] Poll() CONSUMER INVALID: %d frames dropped (no consumer registered)"),
            FramesProcessed);
    }

    // ARCHITECTURE VERIFICATION: Log end of Poll (Verbose)
    UE_LOG(LogO3DWebRTCReceiver, Verbose,
        TEXT("[ARCH] Poll() END: Processed %d frames from %d subjects"),
        FramesProcessed, PendingFramesBySubject.Num());

    double LastDataTime = 0.0;
    {
        FScopeLock DataLock(&LastDataMutex);
        LastDataTime = LastDataReceiveTime;
    }

    if (NoDataReconnectTimeoutSec > 0.0 && (NowSeconds - LastDataTime) > NoDataReconnectTimeoutSec)
    {
        RequestReconnect();
    }

    ApplyPendingAudioFormatIfNeeded();
    ProcessReconnectIfNeeded();

    return FramesProcessed;
}

FO3DTransportStats FO3DWebRTCReceiver::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    // Sync atomic stats into the main stats struct for return (called less frequently)
    Stats.FramesReceived = AtomicFramesReceived.Load();
    Stats.FramesSent = AtomicFramesSent.Load();
    Stats.BytesReceived = AtomicBytesReceived.Load();
    Stats.BytesSent = AtomicBytesSent.Load();
    Stats.DroppedFrames = AtomicDroppedFrames.Load();
    return Stats;
}

void FO3DWebRTCReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
    FScopeLock Lock(&StateMutex);
    AudioSink = Sink;
    ActiveAudioConfig = AudioConfig;

    if (ClientHandle && bInitialized.Load())
    {
        LogIfFailed(lk_set_audio_output_format(ClientHandle, ActiveAudioConfig.SampleRate, ActiveAudioConfig.NumChannels),
            TEXT("LiveKit update audio output format"));
        bPendingAudioFormatApply.Store(false);
    }
    else
    {
        bPendingAudioFormatApply.Store(ActiveAudioConfig.bEnableAudio);
    }
}

bool FO3DWebRTCReceiver::ParseConfig(const FO3DTransportConfig& Config)
{
    FString HostAddress = Config.Uri;
    if (HostAddress.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC host address not specified"));
        return false;
    }

    // Automatically prepend the correct WebSocket protocol prefix
    RoomUrl = WebRTCUtils::PrependWebSocketProtocol(HostAddress);

    Token = Config.Token;
    if (Token.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC token not provided"));
        return false;
    }

    // Use StreamId as subject name if provided, otherwise use a default
    SubjectName = Config.StreamId.IsEmpty() ? TEXT("WebRTCStream") : Config.StreamId;

    const double TimeoutSeconds = FMath::Clamp(ParseDoubleOption(Config.AdvancedParams, ReconnectTimeoutOptionKey, 2.0), 0.0, 300.0);
    NoDataReconnectTimeoutSec = TimeoutSeconds;

    return true;
}

FString FO3DWebRTCReceiver::GetOrCacheSubjectLabel(const char* RawLabel)
{
    // Handle null or empty label
    if (!RawLabel || !*RawLabel)
    {
        return TEXT("default");
    }

    // Compute hash of the C-string
    const uint32 Hash = FCrc::MemCrc32(RawLabel, static_cast<int32>(FCStringAnsi::Strlen(RawLabel)));

    // Check cache (brief lock)
    {
        FScopeLock Lock(&LabelCacheMutex);
        FString* CachedLabel = SubjectLabelCache.Find(Hash);
        if (CachedLabel)
        {
            return *CachedLabel;
        }
    }

    // Not in cache - convert and cache (avoid repeated UTF8→UTF16 conversion)
    FString NewLabel(RawLabel);

    {
        FScopeLock Lock(&LabelCacheMutex);
        // Double-check in case another thread added it
        FString* CachedLabel = SubjectLabelCache.Find(Hash);
        if (CachedLabel)
        {
            return *CachedLabel;
        }
        SubjectLabelCache.Add(Hash, NewLabel);
    }

    return NewLabel;
}

bool FO3DWebRTCReceiver::SetupClientHandle()
{
    EnsureLiveKitFfiLoaded();

    if (ClientHandle)
    {
        return true;
    }

    ClientHandle = lk_client_create();
    if (!ClientHandle)
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to create LiveKit client"));
        return false;
    }

    LogIfFailed(lk_set_log_level(ClientHandle, LkLogInfo), TEXT("LiveKit set log level"));

    LogIfFailed(lk_set_connection_callback(ClientHandle, FO3DWebRTCReceiver::OnConnectionState, this),
        TEXT("LiveKit set connection callback"));

    // DIAGNOSTIC: Log before and after data callback registration
    UE_LOG(LogO3DWebRTCReceiver, Warning,
        TEXT("[DIAG] Registering OnDataReceivedEx callback..."));
    LkResult DataCallbackResult = lk_client_set_data_callback_ex(ClientHandle, FO3DWebRTCReceiver::OnDataReceivedEx, this);
    if (DataCallbackResult.code == 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning,
            TEXT("[DIAG] OnDataReceivedEx callback registered successfully"));
    }
    else
    {
        UE_LOG(LogO3DWebRTCReceiver, Error,
            TEXT("[DIAG] OnDataReceivedEx callback registration FAILED: code=%d"), DataCallbackResult.code);
    }
    LogIfFailed(DataCallbackResult, TEXT("LiveKit set extended data callback"));

    // DIAGNOSTIC: If labeled callback registered successfully, that's good.
    // But also register the UNLABELED callback as a fallback in case server
    // sends data to default channel instead of labeled channels.
    // This helps diagnose if the issue is "labeled channels not supported" vs "other"
    if (DataCallbackResult.code == 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning,
            TEXT("[DIAG] Also registering OnDataReceived fallback (unlabeled) callback for diagnostics..."));
        LkResult FallbackResult = lk_client_set_data_callback(ClientHandle, FO3DWebRTCReceiver::OnDataReceived, this);
        if (FallbackResult.code == 0)
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning,
                TEXT("[DIAG] OnDataReceived fallback callback also registered - will fire if data arrives on default channel"));
        }
        else
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning,
                TEXT("[DIAG] OnDataReceived fallback registration also failed: code=%d"), FallbackResult.code);
        }
    }

    // Use new per-subject audio callback with label support
    UE_LOG(LogO3DWebRTCReceiver, Warning,
        TEXT("[DIAG] Registering OnAudioReceivedEx callback..."));
    LkResult AudioCallbackResult = lk_client_set_audio_callback_ex(ClientHandle, FO3DWebRTCReceiver::OnAudioReceivedEx, this);
    if (AudioCallbackResult.code == 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning,
            TEXT("[DIAG] OnAudioReceivedEx callback registered successfully"));
    }
    else
    {
        UE_LOG(LogO3DWebRTCReceiver, Error,
            TEXT("[DIAG] OnAudioReceivedEx callback registration FAILED: code=%d"), AudioCallbackResult.code);
    }
    LogIfFailed(AudioCallbackResult, TEXT("LiveKit set extended audio callback with per-subject labels"));

    LogIfFailed(lk_set_default_data_labels(ClientHandle, "o3ds-rel", "o3ds-lossy"),
        TEXT("LiveKit set default data labels"));

    if (ActiveAudioConfig.bEnableAudio && ActiveAudioConfig.SampleRate > 0 && ActiveAudioConfig.NumChannels > 0)
    {
        LogIfFailed(lk_set_audio_output_format(ClientHandle, ActiveAudioConfig.SampleRate, ActiveAudioConfig.NumChannels),
            TEXT("LiveKit set audio output format"));
    }

    return true;
}

bool FO3DWebRTCReceiver::BeginConnect()
{
    if (!ClientHandle)
    {
        return false;
    }

    UE_LOG(LogO3DWebRTCReceiver, Warning,
        TEXT("[DIAG] BeginConnect: URL='%s' Token='%s' (first 20 chars)"),
        *RoomUrl, *Token.Left(20));

    LkResult Result = lk_connect_with_role_async(
        ClientHandle,
        TCHAR_TO_UTF8(*RoomUrl),
        TCHAR_TO_UTF8(*Token),
        LkRoleSubscriber);

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to connect: %s"), *FromAnsi(Result.message));
        if (Result.message)
        {
            lk_free_str(const_cast<char*>(Result.message));
        }
        return false;
    }

    UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("[DIAG] BeginConnect succeeded, awaiting async connection..."));
    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connecting..."));
    return true;
}

void FO3DWebRTCReceiver::RequestReconnect(bool bForce)
{
    if (!bForce && NoDataReconnectTimeoutSec <= 0.0)
    {
        return;
    }

    if (!bReconnectPending.Load())
    {
        bReconnectPending.Store(true);
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver scheduling reconnect (force=%d)"), bForce ? 1 : 0);
    }
}

void FO3DWebRTCReceiver::ProcessReconnectIfNeeded()
{
    if (!bReconnectPending.Load())
    {
        return;
    }

    FScopeLock Lock(&StateMutex);
    if (!bReconnectPending.Load())
    {
        return;
    }

    bReconnectPending.Store(false);

    if (ClientHandle)
    {
        lk_client_set_data_callback_ex(ClientHandle, nullptr, nullptr);
        lk_client_set_audio_callback(ClientHandle, nullptr, nullptr);
        lk_set_connection_callback(ClientHandle, nullptr, nullptr);
        LogIfFailed(lk_disconnect(ClientHandle), TEXT("LiveKit reconnect disconnect"));
        lk_client_destroy(ClientHandle);
        ClientHandle = nullptr;
    }

    bConnected.Store(false);

    if (!SetupClientHandle())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver failed to recreate LiveKit client during reconnect"));
        return;
    }

    if (!BeginConnect())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver failed to reconnect; will retry"));
        bReconnectPending.Store(true);
        return;
    }

    {
        FScopeLock PendingLock(&PendingFramesMutex);
        PendingFramesBySubject.Reset();
    }

    {
        FScopeLock DataLock(&LastDataMutex);
        LastDataReceiveTime = FPlatformTime::Seconds();
    }

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver reconnect initiated"));
}

void FO3DWebRTCReceiver::ApplyPendingAudioFormatIfNeeded()
{
    if (!bPendingAudioFormatApply.Load())
    {
        return;
    }

    LkClientHandle* LocalHandle = nullptr;
    FO3DTransportAudioConfig AudioConfigCopy;
    {
        FScopeLock Lock(&StateMutex);
        LocalHandle = ClientHandle;
        AudioConfigCopy = ActiveAudioConfig;
    }

    if (!LocalHandle)
    {
        return;
    }

    if (AudioConfigCopy.bEnableAudio && AudioConfigCopy.SampleRate > 0 && AudioConfigCopy.NumChannels > 0)
    {
        LogIfFailed(lk_set_audio_output_format(LocalHandle, AudioConfigCopy.SampleRate, AudioConfigCopy.NumChannels),
            TEXT("LiveKit reapply audio output format"));
        bPendingAudioFormatApply.Store(false);
        return;
    }

    // Audio disabled (or invalid config) – nothing to apply.
    bPendingAudioFormatApply.Store(false);
}
