// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "WebRTCReceiver.h"
#include "O3DRedact.h"
#include "../Shared/WebRTCUtils.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Math/NumericLimits.h"
#include "Containers/StringConv.h"
#include "O3DFfiContextRegistry.h"
THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

namespace
{
    static constexpr TCHAR ReconnectTimeoutOptionKey[] = TEXT("webrtc.reconnect_timeout");

    // Upper bound on a single incoming data-channel message. `len` originates from the
    // peer-controlled LiveKit FFI callback as a size_t; it must be validated before being
    // truncated to int32 for TArray allocation, otherwise a value exceeding INT32_MAX (or
    // simply an oversized/malicious payload) can cause the allocation to be smaller than the
    // subsequent Memcpy, resulting in a heap buffer overflow. The sender caps reliable
    // messages at 15000 bytes (see WebRTCSender.cpp), so this generous cap leaves headroom
    // while still bounding worst-case allocation/copy size.
    static constexpr size_t MaxIncomingDataPayloadBytes = 8 * 1024 * 1024; // 8 MB
    static_assert(MaxIncomingDataPayloadBytes <= static_cast<size_t>(TNumericLimits<int32>::Max()),
        "MaxIncomingDataPayloadBytes must fit within int32 for TArray allocation");

    /** Label used for frames that arrive on the unlabeled fallback callback or with no label. */
    static constexpr TCHAR DefaultSubjectLabel[] = TEXT("default");

    TO3DFfiContextRegistry<FWebRTCReceiverLink>& GetReceiverLinkRegistry()
    {
        static TO3DFfiContextRegistry<FWebRTCReceiverLink> Registry;
        return Registry;
    }

    void LogIfFailed(const FLkFfiApi& Ffi, const LkResult& Result, const TCHAR* Context)
    {
        if (Result.code == 0)
        {
            return;
        }

        const FString Message = Ffi.TakeMessage(Result);
        if (Message.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("%s failed (code=%d)"), Context, Result.code);
        }
        else
        {
            UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("%s failed (code=%d): %s"), Context, Result.code, *Message);
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

    /** Validates a data-callback payload before it is copied. */
    bool IsAcceptablePayload(const uint8_t* Bytes, size_t Len)
    {
        if (!Bytes || Len == 0)
        {
            return false;
        }

        // SECURITY: `len` is peer-controlled. Reject payloads that would overflow the int32
        // cast used for TArray allocation (or are simply unreasonably large).
        if (Len > MaxIncomingDataPayloadBytes || Len > static_cast<size_t>(TNumericLimits<int32>::Max()))
        {
            UE_LOG(LogO3DWebRTCReceiver, Error,
                TEXT("Rejecting oversized WebRTC data payload len=%zu (max=%zu)"),
                Len, MaxIncomingDataPayloadBytes);
            return false;
        }
        return true;
    }
}

void FWebRTCReceiverLink::EnqueueFrame(const FString& SubjectLabel, const uint8* Bytes, int32 Len)
{
    FWebRTCReceiverPendingFrame Frame;
    Frame.EnqueueTimeSeconds = FPlatformTime::Seconds();
    Frame.Payload.SetNumUninitialized(Len);
    FMemory::Memcpy(Frame.Payload.GetData(), Bytes, Len);

    {
        FScopeLock Lock(&PendingFramesMutex);
        PendingFramesBySubject.FindOrAdd(SubjectLabel).Emplace(MoveTemp(Frame));
    }

    LastDataReceiveTime.store(FPlatformTime::Seconds());
    bReconnectPending.Store(false);
}

void FWebRTCReceiverLink::RequestReconnect()
{
    if (!bReconnectPending.Exchange(true))
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver scheduling reconnect"));
    }
}

// Static callback for connection state changes
void FO3DWebRTCReceiver::OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message)
{
    const TSharedPtr<FWebRTCReceiverLink, ESPMode::ThreadSafe> Self = GetReceiverLinkRegistry().Resolve(user);
    if (!Self.IsValid()) return;

    switch (state)
    {
    case LkConnConnecting:
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connecting..."));
        break;

    case LkConnConnected:
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connected"));
        Self->bConnected.Store(true);
        Self->bPendingAudioFormatApply.Store(true);
        Self->bReconnectPending.Store(false);
        Self->LastDataReceiveTime.store(FPlatformTime::Seconds());
        break;

    case LkConnReconnecting:
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver reconnecting..."));
        Self->bConnected.Store(false);
        break;

    case LkConnDisconnected:
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver disconnected: %s"), *WebRTCUtils::FromAnsi(message));
        Self->bConnected.Store(false);
        Self->RequestReconnect();
        break;

    case LkConnFailed:
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver connection failed (code=%d): %s"), reason_code, *WebRTCUtils::FromAnsi(message));
        Self->bConnected.Store(false);
        Self->RequestReconnect();
        break;
    }
}

// Static callback for incoming data with label and reliability info
void FO3DWebRTCReceiver::OnDataReceivedEx(void* user, const char* label, LkReliability reliability, const uint8_t* bytes, size_t len)
{
    const TSharedPtr<FWebRTCReceiverLink, ESPMode::ThreadSafe> Self = GetReceiverLinkRegistry().Resolve(user);
    if (!Self.IsValid() || !IsAcceptablePayload(bytes, len))
    {
        return;
    }

    // Labels are UTF-8 (the sender encodes them with FTCHARToUTF8): decode, don't widen (TRF-31).
    FString SubjectLabel = WebRTCUtils::DecodeUtf8Label(label);
    if (SubjectLabel.IsEmpty())
    {
        SubjectLabel = DefaultSubjectLabel;
    }

    UE_LOG(LogO3DWebRTCReceiver, VeryVerbose, TEXT("OnDataReceivedEx label='%s' %d bytes (%s)"),
        *SubjectLabel, static_cast<int32>(len), reliability == LkReliable ? TEXT("Reliable") : TEXT("Lossy"));

    Self->EnqueueFrame(SubjectLabel, bytes, static_cast<int32>(len));
}

// FALLBACK: unlabeled data callback, registered only when lk_client_set_data_callback_ex fails.
void FO3DWebRTCReceiver::OnDataReceived(void* user, const uint8_t* bytes, size_t len)
{
    const TSharedPtr<FWebRTCReceiverLink, ESPMode::ThreadSafe> Self = GetReceiverLinkRegistry().Resolve(user);
    if (!Self.IsValid() || !IsAcceptablePayload(bytes, len))
    {
        return;
    }

    Self->EnqueueFrame(DefaultSubjectLabel, bytes, static_cast<int32>(len));
}

// Per-subject audio callback (participant_name and track_name from LiveKit FFI)
void FO3DWebRTCReceiver::OnAudioReceivedEx(void* user, const int16_t* pcm_interleaved, size_t frames_per_channel, int32_t channels, int32_t sample_rate, const char* participant_name, const char* track_name)
{
    const TSharedPtr<FWebRTCReceiverLink, ESPMode::ThreadSafe> Self = GetReceiverLinkRegistry().Resolve(user);
    if (!Self.IsValid() || !pcm_interleaved || frames_per_channel == 0 || channels <= 0 || sample_rate <= 0) return;

    const size_t TotalSamples = frames_per_channel * static_cast<size_t>(channels);
    const size_t NumBytes = TotalSamples * sizeof(int16);
    if (NumBytes > MaxIncomingDataPayloadBytes)
    {
        return;
    }

    Self->FramesReceived.IncrementExchange();
    Self->BytesReceived.AddExchange(static_cast<int64>(NumBytes));

    const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = Self->GetAudioSink();
    if (SinkPinned.IsValid())
    {
        // track_name is the subject label the sender used when it created the track, so audio
        // routes to the same subject as mocap data. Decoded as UTF-8 (TRF-31).
        FString SubjectLabel = WebRTCUtils::DecodeUtf8Label(track_name);
        if (SubjectLabel.IsEmpty())
        {
            SubjectLabel = TEXT("audio_default");
        }

        O3DS::FAudioFrameMeta Meta;
        Meta.SampleRate = sample_rate;
        Meta.NumChannels = channels;
        Meta.StreamLabel = SubjectLabel;

        SinkPinned->SubmitPcm16(Meta, reinterpret_cast<const uint8*>(pcm_interleaved), static_cast<int32>(NumBytes));
    }
    else
    {
        const double Now = FPlatformTime::Seconds();
        double Last = Self->LastAudioDropLogTime.load();
        if (Now - Last > 1.0 && Self->LastAudioDropLogTime.compare_exchange_strong(Last, Now))
        {
            UE_LOG(LogO3DWebRTCReceiver, Verbose, TEXT("WebRTC audio frame discarded (no sink) - participant='%s' track='%s' frames=%d channels=%d sr=%d"),
                *WebRTCUtils::FromAnsi(participant_name), *WebRTCUtils::FromAnsi(track_name), (int32)frames_per_channel, channels, sample_rate);
        }
    }
}

FO3DWebRTCReceiver::FO3DWebRTCReceiver()
    : FO3DWebRTCReceiver(GetLinkedLkFfiApi())
{
}

FO3DWebRTCReceiver::FO3DWebRTCReceiver(const FLkFfiApi& InFfi, FO3DTokenFetcherFactory InTokenFetcherFactory)
    : Ffi(InFfi)
    , TokenFetcherFactory(MoveTemp(InTokenFetcherFactory))
    , Link(MakeShared<FWebRTCReceiverLink, ESPMode::ThreadSafe>())
{
    LinkToken = GetReceiverLinkRegistry().Register(Link);
}

FO3DWebRTCReceiver::~FO3DWebRTCReceiver()
{
    Stop();
    GetReceiverLinkRegistry().Unregister(LinkToken);
    LinkToken = nullptr;
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

    if (!Ffi.IsComplete())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver: LiveKit function table is incomplete"));
        return false;
    }

    Link->bConnected.Store(false);

    if (!ParseConfig(Config))
    {
        return false;
    }

    ActiveAudioConfig = Config.Audio;

    if (!SetupClientHandle())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver: failed to set up the LiveKit client"));
        return false;
    }

    ActiveConfig = Config;
    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.Reset();
        LatencySamples = 0;
    }
    Link->FramesReceived.Store(0);
    Link->BytesReceived.Store(0);
    Link->LastAudioDropLogTime.store(0.0);
    Link->LastDataReceiveTime.store(FPlatformTime::Seconds());

    bConnectRequested = false;
    bConnectIssued = false;
    AppliedTokenGeneration = 0;
    ObservedTokenGeneration = 0;
    NextTokenFetchTime = 0.0;
    NextConnectAttemptTime = 0.0;

    bInitialized.Store(true);

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver initialized: URL=%s"), *O3DRedact::Url(RoomUrl));

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

    // "Connect requested" is tracked separately from the token (TRF-3). Without a token yet,
    // Poll() connects as soon as the token manager has one.
    bConnectRequested = true;
    return UpdateConnection();
}

void FO3DWebRTCReceiver::Stop()
{
    FScopeLock Lock(&StateMutex);

    bConnectRequested = false;
    bConnectIssued = false;

    // Cancel any token fetch; its result is dropped (TRF-24).
    if (TokenManager.IsValid())
    {
        TokenManager->Reset();
    }

    DestroyClientHandle(TEXT("LiveKit disconnect"));

    Consumer.Reset();
    {
        // lk_disconnect/lk_client_destroy above have returned, so no callback is running
        // (livekit_ffi.h: "After lk_disconnect() or lk_client_destroy() returns, no further
        // callbacks will be invoked"). A late callback would resolve Link through the registry,
        // which is still safe.
        FScopeLock SinkLock(&Link->AudioSinkMutex);
        Link->AudioSink.Reset();
    }
    {
        FScopeLock PendingLock(&Link->PendingFramesMutex);
        Link->PendingFramesBySubject.Reset();
    }

    Link->bConnected.Store(false);
    Link->bPendingAudioFormatApply.Store(false);
    Link->bReconnectPending.Store(false);
    bInitialized.Store(false);

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver stopped"));
}

int32 FO3DWebRTCReceiver::Poll()
{
    // Take every queued frame; frames are delivered in arrival order per subject.
    TMap<FString, TArray<FWebRTCReceiverPendingFrame>> AllFramesBySubject;
    {
        FScopeLock Lock(&Link->PendingFramesMutex);
        AllFramesBySubject = MoveTemp(Link->PendingFramesBySubject);
        Link->PendingFramesBySubject.Reset();
    }

    // Snapshot the consumer under the state lock (TRF-15).
    TSharedPtr<ISerializedFrameConsumer> ConsumerSnapshot;
    {
        FScopeLock Lock(&StateMutex);
        ConsumerSnapshot = Consumer;
    }

    int32 FramesProcessed = 0;
    const double NowSeconds = FPlatformTime::Seconds();

    if (ConsumerSnapshot.IsValid())
    {
        for (auto& SubjectEntry : AllFramesBySubject)
        {
            const FString& SubjectLabel = SubjectEntry.Key;
            for (FWebRTCReceiverPendingFrame& Frame : SubjectEntry.Value)
            {
                const double ReceiveLatencyMs = FMath::Max(0.0, (NowSeconds - Frame.EnqueueTimeSeconds) * 1000.0);

                Link->FramesReceived.IncrementExchange();
                Link->BytesReceived.AddExchange(Frame.Payload.Num());

                {
                    FScopeLock Lock(&StatsMutex);
                    Stats.MaxLatencyMs = FMath::Max(Stats.MaxLatencyMs, ReceiveLatencyMs);
                    const int64 NewSampleCount = LatencySamples + 1;
                    const double PreviousTotal = Stats.AverageLatencyMs * LatencySamples;
                    Stats.AverageLatencyMs = (PreviousTotal + ReceiveLatencyMs) / FMath::Max<int64>(1, NewSampleCount);
                    LatencySamples = NewSampleCount;
                }

                // LiveLink expects WorldTime to be "when to display", so submit with the current
                // time rather than the arrival time.
                ConsumerSnapshot->SubmitFrame(SubjectLabel, Frame.Payload, FPlatformTime::Seconds());
                FramesProcessed++;
            }
        }
    }
    else
    {
        int32 Dropped = 0;
        for (const auto& SubjectEntry : AllFramesBySubject)
        {
            Dropped += SubjectEntry.Value.Num();
        }
        if (Dropped > 0)
        {
            UE_LOG(LogO3DWebRTCReceiver, Verbose, TEXT("Poll(): %d frames dropped (no consumer)"), Dropped);
            FScopeLock Lock(&StatsMutex);
            Stats.DroppedFrames += Dropped;
        }
    }

    {
        FScopeLock Lock(&StateMutex);
        UpdateConnection();

        // The no-data watchdog only applies once a connect has been issued; while waiting for a
        // token there is nothing to reconnect.
        if (bConnectIssued && NoDataReconnectTimeoutSec > 0.0 &&
            (NowSeconds - Link->LastDataReceiveTime.load()) > NoDataReconnectTimeoutSec)
        {
            Link->RequestReconnect();
        }
    }

    ApplyPendingAudioFormatIfNeeded();
    ProcessReconnectIfNeeded();

    return FramesProcessed;
}

FO3DTransportStats FO3DWebRTCReceiver::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    Stats.FramesReceived = Link->FramesReceived.Load();
    Stats.BytesReceived = Link->BytesReceived.Load();
    return Stats;
}

void FO3DWebRTCReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
    FScopeLock Lock(&StateMutex);
    {
        FScopeLock SinkLock(&Link->AudioSinkMutex);
        Link->AudioSink = Sink;
    }
    ActiveAudioConfig = AudioConfig;

    if (ClientHandle && bInitialized.Load())
    {
        LogIfFailed(Ffi, Ffi.lk_set_audio_output_format(ClientHandle, ActiveAudioConfig.SampleRate, ActiveAudioConfig.NumChannels),
            TEXT("LiveKit update audio output format"));
        Link->bPendingAudioFormatApply.Store(false);
    }
    else
    {
        Link->bPendingAudioFormatApply.Store(ActiveAudioConfig.bEnableAudio);
    }
}

bool FO3DWebRTCReceiver::ParseConfig(const FO3DTransportConfig& Config)
{
    const FString HostAddress = Config.Uri;
    if (HostAddress.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC host address not specified"));
        return false;
    }

    // Automatically prepend the correct WebSocket protocol prefix
    RoomUrl = WebRTCUtils::PrependWebSocketProtocol(HostAddress);

    TokenManager = MakeUnique<FO3DTokenManager>(TokenFetcherFactory);

    FO3DTokenConfig TokenConfig;

    if (Config.bUseAutoTokenFetch)
    {
        TokenConfig.Mode = EO3DTokenMode::AutoFetch;
        TokenConfig.EndpointUrl = Config.TokenEndpointUrl;
        // Both sides read the room from `webrtc.room` (TRF-25); StreamId is not a room name.
        TokenConfig.RoomName = WebRTCUtils::ResolveRoomName(Config.AdvancedParams);
        TokenConfig.Identity = WebRTCUtils::MakeParticipantIdentity(TEXT("receiver"));
        TokenConfig.Role = EO3DTokenRole::Subscriber;
        TokenConfig.RefreshLeadTimeSec = Config.TokenRefreshLeadTimeSec;
        // Declared secret, resolved into Config.Secrets by the component or source (ADR 0004).
        TokenConfig.EndpointAuth = WebRTCUtils::FindSecret(Config.Secrets, WebRTCUtils::TokenEndpointAuthOptionKey);

        if (TokenConfig.EndpointUrl.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Auto-fetch enabled but no token endpoint URL provided"));
            return false;
        }

        if (TokenConfig.RoomName.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Auto-fetch enabled but no room set (transport option '%s')"), WebRTCUtils::RoomOptionKey);
            return false;
        }

        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("Token auto-fetch enabled: endpoint=%s, room=%s, identity=%s"),
            *O3DRedact::Url(TokenConfig.EndpointUrl), *TokenConfig.RoomName, *TokenConfig.Identity);
    }
    else
    {
        TokenConfig.Mode = EO3DTokenMode::Manual;
        TokenConfig.ManualToken = Config.Token;

        if (TokenConfig.ManualToken.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC token not provided"));
            return false;
        }

        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("Manual token mode"));
    }

    if (!TokenManager->Initialize(TokenConfig))
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to initialize token manager"));
        return false;
    }

    const double TimeoutSeconds = FMath::Clamp(ParseDoubleOption(Config.AdvancedParams, ReconnectTimeoutOptionKey, 2.0), 0.0, 300.0);
    NoDataReconnectTimeoutSec = TimeoutSeconds;

    return true;
}

bool FO3DWebRTCReceiver::SetupClientHandle()
{
    if (ClientHandle)
    {
        return true;
    }

    // livekit_ffi.dll was loaded by the module before this transport was registered (TRF-28).
    ClientHandle = Ffi.lk_client_create();
    if (!ClientHandle)
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to create LiveKit client"));
        return false;
    }

    LogIfFailed(Ffi, Ffi.lk_set_log_level(ClientHandle, LkLogInfo), TEXT("LiveKit set log level"));

    LogIfFailed(Ffi, Ffi.lk_set_connection_callback(ClientHandle, FO3DWebRTCReceiver::OnConnectionState, LinkToken),
        TEXT("LiveKit set connection callback"));

    // Register exactly one data callback (TRF-16). The unlabeled callback is only a fallback for
    // an FFI build without the labeled one; registering both could deliver each packet twice.
    const LkResult DataCallbackResult = Ffi.lk_client_set_data_callback_ex(ClientHandle, FO3DWebRTCReceiver::OnDataReceivedEx, LinkToken);
    if (DataCallbackResult.code != 0)
    {
        LogIfFailed(Ffi, DataCallbackResult, TEXT("LiveKit set extended data callback"));
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("Falling back to the unlabeled data callback; all frames use the '%s' label"), DefaultSubjectLabel);
        LogIfFailed(Ffi, Ffi.lk_client_set_data_callback(ClientHandle, FO3DWebRTCReceiver::OnDataReceived, LinkToken),
            TEXT("LiveKit set data callback"));
    }

    LogIfFailed(Ffi, Ffi.lk_client_set_audio_callback_ex(ClientHandle, FO3DWebRTCReceiver::OnAudioReceivedEx, LinkToken),
        TEXT("LiveKit set extended audio callback"));

    LogIfFailed(Ffi, Ffi.lk_set_default_data_labels(ClientHandle, "o3ds-rel", "o3ds-lossy"),
        TEXT("LiveKit set default data labels"));

    if (ActiveAudioConfig.bEnableAudio && ActiveAudioConfig.SampleRate > 0 && ActiveAudioConfig.NumChannels > 0)
    {
        LogIfFailed(Ffi, Ffi.lk_set_audio_output_format(ClientHandle, ActiveAudioConfig.SampleRate, ActiveAudioConfig.NumChannels),
            TEXT("LiveKit set audio output format"));
    }

    return true;
}

void FO3DWebRTCReceiver::DestroyClientHandle(const TCHAR* Context)
{
    if (!ClientHandle)
    {
        return;
    }

    LogIfFailed(Ffi, Ffi.lk_client_set_data_callback_ex(ClientHandle, nullptr, nullptr), TEXT("LiveKit clear extended data callback"));
    LogIfFailed(Ffi, Ffi.lk_client_set_data_callback(ClientHandle, nullptr, nullptr), TEXT("LiveKit clear data callback"));
    LogIfFailed(Ffi, Ffi.lk_client_set_audio_callback_ex(ClientHandle, nullptr, nullptr), TEXT("LiveKit clear audio callback"));
    LogIfFailed(Ffi, Ffi.lk_set_connection_callback(ClientHandle, nullptr, nullptr), TEXT("LiveKit clear connection callback"));

    LogIfFailed(Ffi, Ffi.lk_disconnect(ClientHandle), Context);

    Ffi.lk_client_destroy(ClientHandle);
    ClientHandle = nullptr;
}

bool FO3DWebRTCReceiver::BeginConnect(const FString& InToken, uint64 TokenGeneration)
{
    if (!ClientHandle)
    {
        return false;
    }

    // Converters live across the FFI call (TRF-2). The token is never logged.
    const FTCHARToUTF8 UrlUtf8(*RoomUrl);
    const FTCHARToUTF8 TokenUtf8(*InToken);
    const LkResult Result = Ffi.lk_connect_with_role_async(ClientHandle, UrlUtf8.Get(), TokenUtf8.Get(), LkRoleSubscriber);

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to connect (code=%d): %s"), Result.code, *Ffi.TakeMessage(Result));
        NextConnectAttemptTime = FPlatformTime::Seconds() + ConnectRetryIntervalSec;
        return false;
    }

    bConnectIssued = true;
    AppliedTokenGeneration = TokenGeneration;
    Link->LastDataReceiveTime.store(FPlatformTime::Seconds());
    Link->bReconnectPending.Store(false);

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver connecting..."));
    return true;
}

bool FO3DWebRTCReceiver::UpdateConnection()
{
    // Called with StateMutex held.
    if (!bInitialized.Load() || !bConnectRequested || !TokenManager.IsValid() || !ClientHandle)
    {
        return true;
    }

    const double Now = FPlatformTime::Seconds();

    FString CurrentToken;
    uint64 Generation = 0;
    const bool bHaveToken = TokenManager->GetCurrentToken(CurrentToken, &Generation);
    if (Generation != ObservedTokenGeneration)
    {
        ObservedTokenGeneration = Generation;
        NextTokenFetchTime = 0.0;
    }

    if (!bConnectIssued)
    {
        if (bHaveToken)
        {
            if (Now < NextConnectAttemptTime)
            {
                return true;
            }
            return BeginConnect(CurrentToken, Generation);
        }

        MaybeFetchToken(Now);
        return true;
    }

    MaybeFetchToken(Now);
    if (bHaveToken && Generation != AppliedTokenGeneration)
    {
        ApplyRefreshedToken(CurrentToken, Generation);
    }
    return true;
}

void FO3DWebRTCReceiver::MaybeFetchToken(double NowSeconds)
{
    if (!TokenManager->IsAutoFetch())
    {
        return;
    }

    if (TokenManager->IsRefreshInProgress())
    {
        // The fetch timeout is enforced here, where it is checked every poll (TRF-24).
        if (NowSeconds - TokenFetchStartTime > TokenFetchTimeoutSec)
        {
            UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Token fetch timed out after %.1f seconds"), TokenFetchTimeoutSec);
            TokenManager->CancelRefresh();
            NextTokenFetchTime = NowSeconds + TokenFetchRetryIntervalSec;
        }
        return;
    }

    if (NowSeconds < NextTokenFetchTime || !TokenManager->NeedsRefresh())
    {
        return;
    }

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("Fetching token..."));
    TokenFetchStartTime = NowSeconds;
    NextTokenFetchTime = NowSeconds + TokenFetchRetryIntervalSec;
    TokenManager->RefreshTokenAsync();
}

void FO3DWebRTCReceiver::ApplyRefreshedToken(const FString& InToken, uint64 TokenGeneration)
{
    AppliedTokenGeneration = TokenGeneration;

    const FTCHARToUTF8 TokenUtf8(*InToken);
    const LkResult Result = Ffi.lk_refresh_token(ClientHandle, TokenUtf8.Get());
    if (Result.code == 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("Applied refreshed LiveKit token"));
        return;
    }

    // livekit_ffi.h: "If not supported, returns error; fallback is disconnect + reconnect."
    // The reconnect path recreates the client and connects with the current token.
    UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("lk_refresh_token failed (code=%d): %s. Reconnecting with the new token."),
        Result.code, *Ffi.TakeMessage(Result));
    Link->RequestReconnect();
}

void FO3DWebRTCReceiver::ProcessReconnectIfNeeded()
{
    if (!Link->bReconnectPending.Load())
    {
        return;
    }

    FScopeLock Lock(&StateMutex);
    if (!Link->bReconnectPending.Exchange(false))
    {
        return;
    }

    if (!bInitialized.Load() || !bConnectRequested)
    {
        return;
    }

    DestroyClientHandle(TEXT("LiveKit reconnect disconnect"));
    Link->bConnected.Store(false);
    bConnectIssued = false;

    {
        FScopeLock PendingLock(&Link->PendingFramesMutex);
        Link->PendingFramesBySubject.Reset();
    }

    if (!SetupClientHandle())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver failed to recreate LiveKit client during reconnect"));
        return;
    }

    // Connect now if a token is available; otherwise UpdateConnection() connects once one is.
    NextConnectAttemptTime = 0.0;
    UpdateConnection();

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver reconnect initiated"));
}

void FO3DWebRTCReceiver::ApplyPendingAudioFormatIfNeeded()
{
    if (!Link->bPendingAudioFormatApply.Load())
    {
        return;
    }

    FScopeLock Lock(&StateMutex);
    if (!ClientHandle)
    {
        return;
    }

    if (ActiveAudioConfig.bEnableAudio && ActiveAudioConfig.SampleRate > 0 && ActiveAudioConfig.NumChannels > 0)
    {
        LogIfFailed(Ffi, Ffi.lk_set_audio_output_format(ClientHandle, ActiveAudioConfig.SampleRate, ActiveAudioConfig.NumChannels),
            TEXT("LiveKit reapply audio output format"));
    }

    // Audio disabled (or invalid config): nothing to apply.
    Link->bPendingAudioFormatApply.Store(false);
}

#endif // O3D_WITH_TRANSPORT_WEBRTC
