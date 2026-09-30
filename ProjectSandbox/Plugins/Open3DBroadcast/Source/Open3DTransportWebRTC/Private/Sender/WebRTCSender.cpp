#include "WebRTCSender.h"
#include "../Shared/WebRTCUtils.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Logging/LogMacros.h"
#include "Containers/StringConv.h"
#include "o3ds/model.h"
#include "O3DPerformanceMetrics.h"
#include "O3DAudioFrameCodec.h"
#include "O3DFfiContextRegistry.h"
#include <vector>

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

namespace
{
    // LiveKit data channel size guidance (livekit_ffi.h, lk_send_data_ex).
    constexpr int32 LossyMaxBytes = 1300;
    constexpr int32 ReliableMaxBytes = 15000;

    TO3DFfiContextRegistry<FWebRTCSenderLink>& GetSenderLinkRegistry()
    {
        static TO3DFfiContextRegistry<FWebRTCSenderLink> Registry;
        return Registry;
    }
}

/**
 * Audio sink implementation for WebRTC sender using LiveKit FFI.
 *
 * Publishes audio to per-subject labeled audio tracks via lk_audio_track_publish_pcm_i16().
 * Each StreamLabel gets its own dedicated audio track, preventing distortion from multiple
 * concurrent audio sources.
 *
 * Lifetime (WP-S5, TRF-1): the sink holds the shared FWebRTCSenderLink, never the sender. Every
 * submit enters the link's gate and keeps it for the track lookup and the publish call, so
 * Stop() (which closes the gate first) cannot destroy a track or the client mid-publish.
 * PCM conversion uses call-local scratch with round-to-nearest (TRF-40).
 */
class FWebRTCSenderAudioSink final : public IO3DSenderAudioSink
{
public:
    explicit FWebRTCSenderAudioSink(TSharedRef<FWebRTCSenderLink, ESPMode::ThreadSafe> InLink)
        : Link(MoveTemp(InLink))
        , BoundEpoch(Link->Gate->GetEpoch())
    {
    }

    virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
    {
        if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
        {
            return false;
        }

        FO3DLifetimeGate::FReadScope Scope(*Link->Gate, BoundEpoch);
        if (!Scope)
        {
            return false;
        }

        if (!Link->ClientHandle || !Link->bConnected.Load())
        {
            return false;
        }

        // Get or create audio track for this subject
        LkAudioTrackHandle* Track = GetOrCreateAudioTrack(StreamLabel, NumChannels, SampleRate);
        if (!Track)
        {
            return false;
        }

        // Call-local scratch: a sink may be fed by several threads and labels (TRF-40).
        // Typical size: 960 samples * 2 channels.
        const int32 TotalSamples = NumFrames * NumChannels;
        TArray<int16, TInlineAllocator<2048>> PcmConversionBuffer;
        PcmConversionBuffer.SetNumUninitialized(TotalSamples);
        O3DAudio::ConvertFloatToPcm16(Interleaved, TotalSamples, PcmConversionBuffer.GetData());

        // Publish to labeled audio track. Still inside the gate, so Track stays valid.
        const LkResult Result = Link->Ffi.lk_audio_track_publish_pcm_i16(
            Track,
            PcmConversionBuffer.GetData(),
            NumFrames
        );

        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning,
                TEXT("Failed to publish audio to track '%s' (frames=%d, ch=%d, sr=%d): %s"),
                *StreamLabel, NumFrames, NumChannels, SampleRate, *Link->Ffi.TakeMessage(Result));
            return false;
        }

        return true;
    }

    virtual void OnCaptureStopped() override
    {
        // No cleanup needed (tracks cleaned up in FO3DWebRTCSender::Stop())
    }

private:
    TSharedRef<FWebRTCSenderLink, ESPMode::ThreadSafe> Link;
    const uint64 BoundEpoch;

    /**
     * Gets existing audio track for StreamLabel, or creates one if it doesn't exist.
     * Called inside the gate; the map itself is guarded by Link->AudioTracksMutex.
     */
    LkAudioTrackHandle* GetOrCreateAudioTrack(const FString& StreamLabel, int32 NumChannels, int32 SampleRate)
    {
        FScopeLock Lock(&Link->AudioTracksMutex);

        // Check if track already exists for this subject
        LkAudioTrackHandle* const* ExistingTrack = Link->AudioTracks.Find(StreamLabel);
        if (ExistingTrack && *ExistingTrack)
        {
            return *ExistingTrack;
        }

        // The converter must outlive lk_audio_track_create, which reads track_name (TRF-2).
        const FTCHARToUTF8 TrackNameUtf8(*StreamLabel);
        LkAudioTrackConfig TrackConfig;
        TrackConfig.track_name = TrackNameUtf8.Get();
        TrackConfig.sample_rate = SampleRate;
        TrackConfig.channels = NumChannels;
        TrackConfig.buffer_ms = 100; // 100ms buffer for smooth audio streaming

        LkAudioTrackHandle* NewTrack = nullptr;
        const LkResult Result = Link->Ffi.lk_audio_track_create(
            Link->ClientHandle,
            &TrackConfig,
            &NewTrack
        );

        if (Result.code != 0 || !NewTrack)
        {
            UE_LOG(LogO3DWebRTCSender, Error,
                TEXT("Failed to create audio track for '%s' (ch=%d, sr=%d): %s"),
                *StreamLabel, NumChannels, SampleRate, *Link->Ffi.TakeMessage(Result));
            return nullptr;
        }

        // Store and return new track
        Link->AudioTracks.Add(StreamLabel, NewTrack);
        UE_LOG(LogO3DWebRTCSender, Log,
            TEXT("Created audio track '%s' (ch=%d, sr=%d kHz, buf=100ms)"),
            *StreamLabel, NumChannels, SampleRate / 1000);

        return NewTrack;
    }
};

// Static callback for connection state changes
void FO3DWebRTCSender::OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message)
{
    // `user` is an opaque token (WP-S5); it resolves to the shared link, never to the sender.
    const TSharedPtr<FWebRTCSenderLink, ESPMode::ThreadSafe> Self = GetSenderLinkRegistry().Resolve(user);
    if (!Self.IsValid()) return;

    bool bNewConnectedState = false;

    switch (state)
    {
    case LkConnConnecting:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC connecting..."));
        break;

    case LkConnConnected:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC connected"));
        bNewConnectedState = true;
        break;

    case LkConnReconnecting:
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("WebRTC reconnecting..."));
        break;

    case LkConnDisconnected:
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC disconnected: %s"), *WebRTCUtils::FromAnsi(message));
        break;

    case LkConnFailed:
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC connection failed (code=%d): %s"), reason_code, *WebRTCUtils::FromAnsi(message));
        break;
    }

    Self->bConnected.Store(bNewConnectedState);
    FO3DPerformanceMetrics::Get().SetTransportConnected(TEXT("WebRTC"), bNewConnectedState);
}

FO3DWebRTCSender::FO3DWebRTCSender()
    : FO3DWebRTCSender(GetLinkedLkFfiApi())
{
}

FO3DWebRTCSender::FO3DWebRTCSender(const FLkFfiApi& InFfi, FO3DTokenFetcherFactory InTokenFetcherFactory)
    : Ffi(InFfi)
    , TokenFetcherFactory(MoveTemp(InTokenFetcherFactory))
    , Link(MakeShared<FWebRTCSenderLink, ESPMode::ThreadSafe>(InFfi))
{
    LinkToken = GetSenderLinkRegistry().Register(Link);
}

FO3DWebRTCSender::~FO3DWebRTCSender()
{
    Stop();
    GetSenderLinkRegistry().Unregister(LinkToken);
    LinkToken = nullptr;
}

bool FO3DWebRTCSender::Initialize(const FO3DTransportConfig& Config)
{
    FScopeLock Lock(&StateMutex);

    if (bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC sender already initialized"));
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

    if (!Ffi.IsComplete())
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC sender: LiveKit function table is incomplete"));
        return false;
    }

    if (!ParseConfig(Config))
    {
        return false;
    }

    // Create LiveKit client handle
    ClientHandle = Ffi.lk_client_create();
    if (!ClientHandle)
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to create LiveKit client"));
        return false;
    }

    // Set connection callback
    LkResult Result = Ffi.lk_set_connection_callback(ClientHandle, FO3DWebRTCSender::OnConnectionState, LinkToken);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to set connection callback: %s"), *Ffi.TakeMessage(Result));
    }

    // Configure audio if enabled
    if (Config.Audio.bEnableAudio)
    {
        ActiveAudioConfig = Config.Audio;

        const int32 BitrateKbps = FMath::Clamp(ActiveAudioConfig.BitrateKbps, 16, 128);
        Result = Ffi.lk_set_audio_publish_options(ClientHandle, BitrateKbps * 1000, 0, ActiveAudioConfig.NumChannels > 1 ? 1 : 0);
        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to set audio options: %s"), *Ffi.TakeMessage(Result));
        }
    }

    ActiveConfig = Config;
    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.Reset();
    }

    // Publish the client handle to audio sinks, then open a new audio epoch (WP-S5).
    Link->ClientHandle = ClientHandle;
    Link->Gate->Open();

    bConnectRequested = false;
    bConnectIssued = false;
    AppliedTokenGeneration = 0;
    ObservedTokenGeneration = 0;
    NextTokenFetchTime = 0.0;
    NextConnectAttemptTime = 0.0;

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

    // "Connect requested" is tracked separately from the token (TRF-3). If no token is
    // available yet, Tick() connects as soon as the token manager has one.
    bConnectRequested = true;
    return UpdateConnection();
}

void FO3DWebRTCSender::Stop()
{
    FScopeLock Lock(&StateMutex);

    // WP-S5 (TRF-1): mark the audio path invalid before any handle is destroyed. Close()
    // returns once every publish already inside a sink has finished.
    Link->Gate->Close();

    bConnectRequested = false;
    bConnectIssued = false;

    // Cancel any token fetch; its result is dropped (TRF-24).
    if (TokenManager.IsValid())
    {
        TokenManager->Reset();
    }

    if (!ClientHandle)
    {
        bInitialized.Store(false);
        return;
    }

    // Clean up all audio tracks before disconnecting
    {
        FScopeLock AudioLock(&Link->AudioTracksMutex);
        for (auto& TrackEntry : Link->AudioTracks)
        {
            if (TrackEntry.Value)
            {
                const LkResult Result = Ffi.lk_audio_track_destroy(TrackEntry.Value);
                if (Result.code != 0)
                {
                    UE_LOG(LogO3DWebRTCSender, Warning,
                        TEXT("Failed to destroy audio track '%s': %s"),
                        *TrackEntry.Key, *Ffi.TakeMessage(Result));
                }
                TrackEntry.Value = nullptr;
            }
        }
        Link->AudioTracks.Reset();
    }

    if (Link->bConnected.Load())
    {
        const LkResult Result = Ffi.lk_disconnect(ClientHandle);
        if (Result.code != 0)
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Disconnect warning: %s"), *Ffi.TakeMessage(Result));
        }
    }

    // Unregister the connection callback before destroying the client.
    Ffi.TakeMessage(Ffi.lk_set_connection_callback(ClientHandle, nullptr, nullptr));

    Ffi.lk_client_destroy(ClientHandle);
    ClientHandle = nullptr;
    Link->ClientHandle = nullptr;

    Link->bConnected.Store(false);
    bInitialized.Store(false);

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender stopped"));
}

void FO3DWebRTCSender::RecordDroppedFrame()
{
    FO3DPerformanceMetrics::Get().RecordFrameDropped();
    FScopeLock Lock(&StatsMutex);
    Stats.DroppedFrames++;
}

bool FO3DWebRTCSender::Send(const O3DS::SubjectList& List)
{
    // TRF-4/TRF-19: the old per-subject pooled path pushed the caller's Transform pointers into
    // a pooled Subject and could delete them on an early return. This path serializes the
    // caller's list once, borrowing nothing, then sends the bytes like SendSerialized().
    if (!Link->bConnected.Load())
    {
        RecordDroppedFrame();
        return false;
    }

    FO3DPerformanceMetrics::Get().RecordFrameCaptured();
    FO3DPerformanceMetrics::Get().SetActiveSubjectCount(static_cast<int32>(List.mItems.size()));

    std::vector<char> Buffer;
    const double TimestampSeconds = FPlatformTime::Seconds();
    // SubjectList::Serialize is not const; the NNG sender uses the same cast.
    const int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
    if (BytesWritten <= 0)
    {
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC sender failed to serialize subject list"));
        FO3DPerformanceMetrics::Get().RecordSerializationError();
        RecordDroppedFrame();
        return false;
    }

    FO3DPerformanceMetrics::Get().RecordBytesSerialized(BytesWritten);

    // Label: first subject's name, decoded from UTF-8 (TRF-31).
    FString SubjectLabel;
    if (!List.mItems.empty() && List.mItems[0])
    {
        SubjectLabel = WebRTCUtils::DecodeUtf8Label(List.mItems[0]->mName.c_str());
    }

    const bool bSucceeded = SendBytes(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten, SubjectLabel);
    if (!bSucceeded)
    {
        FScopeLock Lock(&StatsMutex);
        Stats.DroppedFrames++;
    }
    return bSucceeded;
}

bool FO3DWebRTCSender::SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double /*CaptureTimestampSec*/)
{
    if (!Link->bConnected.Load())
    {
        RecordDroppedFrame();
        return false;
    }

    if (!Data || Len <= 0)
    {
        return false;
    }

    FO3DPerformanceMetrics::Get().RecordFrameCaptured();
    FO3DPerformanceMetrics::Get().RecordBytesSerialized(Len);

    const bool bSucceeded = SendBytes(Data, Len, SubjectName);
    if (!bSucceeded)
    {
        FScopeLock Lock(&StatsMutex);
        Stats.DroppedFrames++;
    }

    return bSucceeded;
}

/** Sends one serialized payload over a labeled LiveKit data channel. Backpressure is whatever
 *  lk_send_data_ex reports: the heuristic pending-frame estimator was removed (TRF-5). */
bool FO3DWebRTCSender::SendBytes(const uint8* Data, int32 Len, const FString& SubjectName)
{
    const bool bAllowLossy = bPreferLossyData;
    LkReliability Reliability = bAllowLossy ? LkLossy : LkReliable;

    if (bAllowLossy && Len > LossyMaxBytes)
    {
        if (Len <= ReliableMaxBytes)
        {
            Reliability = LkReliable;
        }
        else
        {
            UE_LOG(LogO3DWebRTCSender, Error,
                TEXT("Subject '%s' payload size (%d bytes) exceeds maximum (%d bytes), consider simplifying skeleton"),
                *SubjectName, Len, ReliableMaxBytes);
            return false;
        }
    }
    else if (!bAllowLossy && Len > ReliableMaxBytes)
    {
        UE_LOG(LogO3DWebRTCSender, Error,
            TEXT("Subject '%s' payload size (%d bytes) exceeds maximum (%d bytes), consider simplifying skeleton"),
            *SubjectName, Len, ReliableMaxBytes);
        return false;
    }

    const FString SubjectLabel = SubjectName.IsEmpty() ? FString(TEXT("subject_0")) : SubjectName;

    // The converter lives until the end of this function, across the FFI call (TRF-2, TRF-31).
    const FTCHARToUTF8 LabelUtf8(*SubjectLabel);
    const LkResult Result = Ffi.lk_send_data_ex(
        ClientHandle,
        Data,
        static_cast<size_t>(Len),
        Reliability,
        1, // ordered = true (preserve frame order)
        LabelUtf8.Get()
    );

    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("Failed to send subject '%s' (code=%d): %s"),
            *SubjectLabel, Result.code, *Ffi.TakeMessage(Result));
        FO3DPerformanceMetrics::Get().RecordTransportFrameDropped();
        return false;
    }

    FO3DPerformanceMetrics::Get().RecordBytesSent(Len);
    FO3DPerformanceMetrics::Get().RecordTransportFrameSent(TEXT("WebRTC"), Len);

    {
        FScopeLock Lock(&StatsMutex);
        Stats.FramesSent++;
        Stats.BytesSent += Len;
    }

    return true;
}

void FO3DWebRTCSender::Tick(float DeltaSeconds)
{
    FScopeLock Lock(&StateMutex);
    UpdateConnection();
}

bool FO3DWebRTCSender::UpdateConnection()
{
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
        // A new token arrived: a later refresh need not wait for the failure retry delay.
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
            return TryConnect(CurrentToken, Generation, Now);
        }

        MaybeFetchToken(Now);
        return true;
    }

    // Connected or connecting: keep the token fresh and hand new tokens to LiveKit (TRF-23).
    MaybeFetchToken(Now);
    if (bHaveToken && Generation != AppliedTokenGeneration)
    {
        ApplyRefreshedToken(CurrentToken, Generation);
    }
    return true;
}

void FO3DWebRTCSender::MaybeFetchToken(double NowSeconds)
{
    if (!TokenManager->IsAutoFetch())
    {
        return;
    }

    if (TokenManager->IsRefreshInProgress())
    {
        // The fetch timeout is enforced here, where it is checked every tick (TRF-24).
        if (NowSeconds - TokenFetchStartTime > TokenFetchTimeoutSec)
        {
            UE_LOG(LogO3DWebRTCSender, Error, TEXT("Token fetch timed out after %.1f seconds"), TokenFetchTimeoutSec);
            TokenManager->CancelRefresh();
            NextTokenFetchTime = NowSeconds + TokenFetchRetryIntervalSec;
        }
        return;
    }

    if (NowSeconds < NextTokenFetchTime || !TokenManager->NeedsRefresh())
    {
        return;
    }

    UE_LOG(LogO3DWebRTCSender, Log, TEXT("Fetching token..."));
    TokenFetchStartTime = NowSeconds;
    // Cleared early when a new token generation is observed; otherwise this spaces out
    // retries after a failed fetch.
    NextTokenFetchTime = NowSeconds + TokenFetchRetryIntervalSec;
    TokenManager->RefreshTokenAsync();
}

bool FO3DWebRTCSender::TryConnect(const FString& InToken, uint64 TokenGeneration, double NowSeconds)
{
    // Converters live across the FFI call (TRF-2).
    const FTCHARToUTF8 UrlUtf8(*RoomUrl);
    const FTCHARToUTF8 TokenUtf8(*InToken);

    const LkResult Result = Ffi.lk_connect_with_role_async(ClientHandle, UrlUtf8.Get(), TokenUtf8.Get(), LkRolePublisher);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to connect (code=%d): %s"), Result.code, *Ffi.TakeMessage(Result));
        NextConnectAttemptTime = NowSeconds + ConnectRetryIntervalSec;
        return false;
    }

    bConnectIssued = true;
    AppliedTokenGeneration = TokenGeneration;
    UE_LOG(LogO3DWebRTCSender, Log, TEXT("WebRTC sender connecting..."));
    return true;
}

void FO3DWebRTCSender::ApplyRefreshedToken(const FString& InToken, uint64 TokenGeneration)
{
    AppliedTokenGeneration = TokenGeneration;

    const FTCHARToUTF8 TokenUtf8(*InToken);
    const LkResult Result = Ffi.lk_refresh_token(ClientHandle, TokenUtf8.Get());
    if (Result.code == 0)
    {
        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Applied refreshed LiveKit token"));
        return;
    }

    // The header says the fallback is disconnect + reconnect. Tearing down the sender's
    // session would also destroy its audio tracks, so the sender keeps the session and the
    // new token is used on the next connect (see WP-S7 notes).
    UE_LOG(LogO3DWebRTCSender, Warning,
        TEXT("lk_refresh_token failed (code=%d): %s. The new token will be used on the next connect."),
        Result.code, *Ffi.TakeMessage(Result));
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
    const int32 BitrateKbps = FMath::Clamp(ActiveAudioConfig.BitrateKbps, 16, 128);
    const LkResult Result = Ffi.lk_set_audio_publish_options(ClientHandle, BitrateKbps * 1000, 0, ActiveAudioConfig.NumChannels > 1 ? 1 : 0);
    if (Result.code != 0)
    {
        UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to update audio options: %s"), *Ffi.TakeMessage(Result));
    }

    return MakeShared<FWebRTCSenderAudioSink, ESPMode::ThreadSafe>(Link);
}

bool FO3DWebRTCSender::ParseConfig(const FO3DTransportConfig& Config)
{
    const FString HostAddress = Config.Uri;
    if (HostAddress.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC host address not specified"));
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
        TokenConfig.Identity = WebRTCUtils::MakeParticipantIdentity(TEXT("sender"));
        TokenConfig.Role = EO3DTokenRole::Publisher;
        TokenConfig.RefreshLeadTimeSec = Config.TokenRefreshLeadTimeSec;

        if (TokenConfig.EndpointUrl.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCSender, Error, TEXT("Auto-fetch enabled but no token endpoint URL provided"));
            return false;
        }

        if (TokenConfig.RoomName.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCSender, Error, TEXT("Auto-fetch enabled but no room set (transport option '%s')"), WebRTCUtils::RoomOptionKey);
            return false;
        }

        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Token auto-fetch enabled: endpoint=%s, room=%s, identity=%s"),
            *TokenConfig.EndpointUrl, *TokenConfig.RoomName, *TokenConfig.Identity);
    }
    else
    {
        TokenConfig.Mode = EO3DTokenMode::Manual;
        TokenConfig.ManualToken = Config.Token;

        if (TokenConfig.ManualToken.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC token not provided"));
            return false;
        }

        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Manual token mode"));
    }

    if (!TokenManager->Initialize(TokenConfig))
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to initialize token manager"));
        return false;
    }

    bPreferLossyData = WebRTCOptions::ParseBool(Config.AdvancedParams, WebRTCOptions::PreferLossyOptionKey, /*DefaultValue=*/false);

    return true;
}
