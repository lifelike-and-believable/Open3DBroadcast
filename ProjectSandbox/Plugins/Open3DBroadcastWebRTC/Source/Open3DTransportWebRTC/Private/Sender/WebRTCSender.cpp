// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

#include "WebRTCSender.h"
#include "O3DRedact.h"
#include "../Shared/WebRTCUtils.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "Logging/LogMacros.h"
#include "Containers/StringConv.h"
#include "O3DPerformanceMetrics.h"
#include "O3DAudioFrameCodec.h"
#include "O3DFfiContextRegistry.h"
#include "O3DLifetimeGate.h"
#include "O3DUnifiedMessage.h"
#include <vector>

namespace
{
    // LiveKit data channel size guidance (livekit_ffi.h, lk_send_data_ex); shared with the
    // capability query (WebRTCUtils::GetCapabilities).
    constexpr int32 LossyMaxBytes = WebRTCUtils::LossyMaxDataBytes;
    constexpr int32 ReliableMaxBytes = WebRTCUtils::ReliableMaxDataBytes;

    // ADR 0011 item 4: a control envelope fits the lossy limit too, so it never needs splitting.
    static_assert(WebRTCUtils::MaxControlEnvelopeBytes <= LossyMaxBytes, "Control envelopes must fit one LiveKit data message");

    /** Seconds between control send-failure warnings (per sender). */
    constexpr double ControlErrorLogIntervalSec = 2.0;

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
    Self->LkReasonCode.store(static_cast<int32>(reason_code));
    Self->LkState.store(static_cast<int32>(state)); // read by the sender's Tick (ADR 0007 item 3)
    if (Self->TransportMetrics.IsValid())
    {
        Self->TransportMetrics->SetConnected(bNewConnectedState);
    }
}

FO3DWebRTCSender::FO3DWebRTCSender()
    : FO3DWebRTCSender(GetLinkedLkFfiApi())
{
}

FO3DWebRTCSender::FO3DWebRTCSender(const FLkFfiApi& InFfi, FO3DTokenFetcherFactory InTokenFetcherFactory)
    : Ffi(InFfi)
    , TokenFetcherFactory(MoveTemp(InTokenFetcherFactory))
    , Link(MakeShared<FWebRTCSenderLink, ESPMode::ThreadSafe>(InFfi))
    , Context(FO3DRuntimeContext::Default())
    , TransportMetrics(Context->GetMetrics().AcquireTransportMetrics(TEXT("WebRTC")))
    , SenderMetrics(Context->GetMetrics().AcquireSenderMetrics(TEXT("WebRTC sender")))
{
    LinkToken = GetSenderLinkRegistry().Register(Link);
}

FO3DWebRTCSender::~FO3DWebRTCSender()
{
    Stop();
    GetSenderLinkRegistry().Unregister(LinkToken);
    LinkToken = nullptr;
}

FO3DTransportResult FO3DWebRTCSender::Initialize(const FO3DTransportConfig& Config)
{
    FScopeLock Lock(&StateMutex);

    if (bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC sender already initialized"));
        return FO3DTransportResult::Error(EO3DTransportError::Internal, TEXT("WebRTC sender is already initialized; Stop() it first."));
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
    return FO3DTransportResult::Error(EO3DTransportError::Unsupported, TEXT("The WebRTC transport needs Windows 64-bit."));
#endif

    if (!Ffi.IsComplete())
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("WebRTC sender: LiveKit function table is incomplete"));
        return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("The LiveKit library is not loaded or is incomplete."));
    }

    const FO3DTransportResult ConfigResult = ParseConfig(Config);
    if (!ConfigResult.IsOk())
    {
        return ConfigResult;
    }

    Context = FO3DRuntimeContext::OrDefault(Config.Context);
    TransportMetrics = Context->GetMetrics().AcquireTransportMetrics(TEXT("WebRTC"));
    if (!Context->ResolveSenderMetrics(Config.SenderMetrics, TEXT("WebRTC sender"), SenderMetrics))
    {
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("WebRTC sender: the sender metrics handle belongs to another runtime context."));
    }
    Link->TransportMetrics = TransportMetrics; // before the connection callback is registered below

    // Create LiveKit client handle
    ClientHandle = Ffi.lk_client_create();
    if (!ClientHandle)
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to create LiveKit client"));
        return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("Failed to create the LiveKit client."));
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
        *O3DRedact::Url(RoomUrl),
        ActiveAudioConfig.bEnableAudio ? TEXT("true") : TEXT("false"),
        bPreferLossyData ? TEXT("true") : TEXT("false"));

    return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DWebRTCSender::Start()
{
    FScopeLock Lock(&StateMutex);

    if (!bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Cannot start WebRTC sender: not initialized"));
        return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("WebRTC sender Start() before a successful Initialize()."));
    }

    // "Connect requested" is tracked separately from the token (TRF-3). If no token is
    // available yet, Tick() connects as soon as the token manager has one.
    bConnectRequested = true;
    AppliedLkState = -1;
    bEverConnected = false;
    Link->LkState.store(-1);
    ConnectionState.Begin(EO3DConnectionState::Connecting);
    if (!UpdateConnection())
    {
        const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ConnectFailed,
            FString::Printf(TEXT("LiveKit refused the connect to %s."), *O3DRedact::Url(RoomUrl)));
        ConnectionState.End(EO3DConnectionState::Failed, Result);
        bConnectRequested = false;
        return Result;
    }
    bRunning.store(true);
    return FO3DTransportResult::Ok();
}

void FO3DWebRTCSender::Stop()
{
    FScopeLock Lock(&StateMutex);

    // WP-S5 (TRF-1): mark the audio path invalid before any handle is destroyed. Close()
    // returns once every publish already inside a sink has finished.
    Link->Gate->Close();

    bRunning.store(false);
    bConnectRequested = false;
    bConnectIssued = false;
    // Nothing reports a change after Stop: Tick translates LiveKit states only while connecting
    // is requested, and the tracker ignores changes once its session ended.
    ConnectionState.End(EO3DConnectionState::Idle);

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
    SenderMetrics->RecordFrameDropped();
    FScopeLock Lock(&StatsMutex);
    Stats.DroppedFrames++;
}

EO3DSendResult FO3DWebRTCSender::SendSerialized(FO3DSendPayload&& Payload)
{
    if (!Link->bConnected.Load())
    {
        // Counted as dropped, as before: the pose pipeline keeps producing frames while LiveKit
        // connects or reconnects.
        RecordDroppedFrame();
        return bRunning.load() ? EO3DSendResult::NotConnected : EO3DSendResult::NotRunning;
    }

    const int32 Len = Payload.Bytes.Num();
    if (Len <= 0)
    {
        return EO3DSendResult::Invalid;
    }

    SenderMetrics->RecordFrameCaptured();
    SenderMetrics->RecordBytesSerialized(Len);

    // Handed to LiveKit on the caller's thread (TRF-5). LiveKit buffers the message in the data
    // channel and refuses it when it cannot take it, and that refusal is the backpressure the
    // caller sees at once (DroppedBackpressure, counted here). WP-A1 PR 4f keeps this rather than
    // putting an FO3DSendQueue and worker in front: see the ADR 0007 addendum "implementation
    // notes (WP-A1 PR 4f)". livekit_ffi.h does not say whether lk_send_data_ex can block
    // (needs-FFI-verification).
    const EO3DSendResult Result = SendBytes(Payload.Bytes.GetData(), Len, Payload.Subject);
    if (Result != EO3DSendResult::Queued)
    {
        FScopeLock Lock(&StatsMutex);
        Stats.DroppedFrames++;
    }

    return Result;
}

/** Sends one serialized payload over a labeled LiveKit data channel. Backpressure is whatever
 *  lk_send_data_ex reports: the heuristic pending-frame estimator was removed (TRF-5). */
EO3DSendResult FO3DWebRTCSender::SendBytes(const uint8* Data, int32 Len, const FString& SubjectName)
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
            return EO3DSendResult::TooLarge;
        }
    }
    else if (!bAllowLossy && Len > ReliableMaxBytes)
    {
        UE_LOG(LogO3DWebRTCSender, Error,
            TEXT("Subject '%s' payload size (%d bytes) exceeds maximum (%d bytes), consider simplifying skeleton"),
            *SubjectName, Len, ReliableMaxBytes);
        return EO3DSendResult::TooLarge;
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
        SenderMetrics->RecordTransportFrameDropped();
        {
            FScopeLock Lock(&StatsMutex);
            Stats.SendErrors++;
        }
        // LiveKit refused the message (its data channel buffer is full, or the room is closing).
        return EO3DSendResult::DroppedBackpressure;
    }

    SenderMetrics->RecordBytesSent(Len);
    TransportMetrics->RecordFrameSent(static_cast<uint64>(Len));

    {
        FScopeLock Lock(&StatsMutex);
        Stats.FramesSent++;
        Stats.BytesSent += Len;
    }

    return EO3DSendResult::Queued;
}

/**
 * Control (ADR 0011 item 7): reliable and ordered on the `__o3d.ctl` data channel, never through
 * SendSerialized, so control is not a frame and moves no frame, byte or drop counter. The sender
 * always joins as a publisher (LkRolePublisher), so the "refuse while subscriber-only" rule has
 * no case to handle here.
 */
EO3DSendResult FO3DWebRTCSender::SendControl(const uint8* Envelope, int32 Len)
{
    // The gate keeps ClientHandle alive for the call: Stop() closes it before destroying the
    // client, and a closed gate (before Initialize, after Stop) refuses at once without blocking.
    FO3DLifetimeGate::FReadScope Scope(*Link->Gate, Link->Gate->GetEpoch());
    if (!Scope || !bRunning.load())
    {
        return EO3DSendResult::NotRunning;
    }

    // Exactly one well-formed control envelope within the budget; nothing trailing. A well-formed
    // envelope always fits the 1,100-byte budget (ADR 0011 item 4), so oversize bytes are Invalid.
    TConstArrayView<uint8> Payload;
    if (!Envelope || Len <= 0 || Len > WebRTCUtils::MaxControlEnvelopeBytes
        || !O3DS::TryGetControlPayload(Envelope, Len, Payload)
        || Len != static_cast<int32>(Payload.GetData() - Envelope) + Payload.Num())
    {
        return EO3DSendResult::Invalid;
    }

    if (!Link->ClientHandle || !Link->bConnected.Load())
    {
        return EO3DSendResult::NotConnected;
    }

    const LkResult Result = Ffi.lk_send_data_ex(
        Link->ClientHandle,
        Envelope,
        static_cast<size_t>(Len),
        LkReliable,
        1, // ordered
        WebRTCUtils::ControlDataLabelUtf8);

    if (Result.code != 0)
    {
        const FString Message = Ffi.TakeMessage(Result);
        const double Now = FPlatformTime::Seconds();
        double Last = LastControlErrorLogTime.load();
        if (Now - Last >= ControlErrorLogIntervalSec && LastControlErrorLogTime.compare_exchange_strong(Last, Now))
        {
            UE_LOG(LogO3DWebRTCSender, Warning, TEXT("Failed to send a control envelope (code=%d): %s"), Result.code, *Message);
        }
        return EO3DSendResult::DroppedBackpressure;
    }
    return EO3DSendResult::Queued;
}

void FO3DWebRTCSender::Tick(float DeltaSeconds)
{
    FScopeLock Lock(&StateMutex);
    UpdateConnection();
    if (bConnectRequested)
    {
        UpdateConnectionState();
    }
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
    FO3DTransportStats Copy;
    {
        FScopeLock Lock(&StatsMutex);
        Copy = Stats;
    }
    Copy.State = ConnectionState.Get();
    return Copy;
}

FO3DTransportCapabilities FO3DWebRTCSender::GetCapabilities() const
{
    FO3DTransportCapabilities Caps = WebRTCUtils::GetCapabilities(FO3DTransportConfig());
    if (bLossyCapabilities.load())
    {
        Caps.Delivery = EO3DDeliveryGuarantee::Unreliable;
    }
    return Caps;
}

void FO3DWebRTCSender::UpdateConnectionState()
{
    // LiveKit reports connection changes on its own threads, which reach only the shared link;
    // Tick turns the latest one into a connection-state change on the game thread.
    const int32 LkState = Link->LkState.load();
    if (LkState == AppliedLkState)
    {
        return;
    }
    AppliedLkState = LkState;

    switch (LkState) // -1 (nothing reported yet) matches no case
    {
    case static_cast<int32>(LkConnConnecting):
        ConnectionState.Set(bEverConnected ? EO3DConnectionState::Reconnecting : EO3DConnectionState::Connecting);
        break;
    case static_cast<int32>(LkConnConnected):
        bEverConnected = true;
        ConnectionState.Set(EO3DConnectionState::Connected);
        break;
    case static_cast<int32>(LkConnReconnecting):
        ConnectionState.Set(EO3DConnectionState::Reconnecting,
            FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, TEXT("LiveKit is reconnecting.")));
        break;
    case static_cast<int32>(LkConnDisconnected):
        // The sender does not reconnect a closed room by itself: Stop and Start to retry.
        ConnectionState.Set(EO3DConnectionState::Failed,
            FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, TEXT("LiveKit disconnected.")));
        break;
    case static_cast<int32>(LkConnFailed):
        ConnectionState.Set(EO3DConnectionState::Failed,
            FO3DTransportResult::Error(EO3DTransportError::ConnectFailed,
                FString::Printf(TEXT("LiveKit connection failed (code=%d)."), Link->LkReasonCode.load())));
        break;
    default:
        break;
    }
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

FO3DTransportResult FO3DWebRTCSender::ParseConfig(const FO3DTransportConfig& Config)
{
    const FString HostAddress = Config.Uri;
    if (HostAddress.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC host address not specified"));
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("WebRTC sender: no LiveKit server address."));
    }

    // Automatically prepend the correct WebSocket protocol prefix
    RoomUrl = WebRTCUtils::PrependWebSocketProtocol(HostAddress);

    TokenManager = MakeUnique<FO3DTokenManager>(TokenFetcherFactory);

    FO3DTokenConfig TokenConfig;
    // From the options and Config.Secrets (WP-A1 PR 5a; they were FO3DTransportConfig fields).
    const WebRTCUtils::FTokenSettings TokenSettings = WebRTCUtils::ReadTokenSettings(Config);

    if (TokenSettings.bAutoFetch)
    {
        TokenConfig.Mode = EO3DTokenMode::AutoFetch;
        TokenConfig.EndpointUrl = TokenSettings.EndpointUrl;
        // Both sides read the room from `webrtc.room` (TRF-25); StreamId is not a room name.
        TokenConfig.RoomName = WebRTCUtils::ResolveRoomName(Config.AdvancedParams);
        TokenConfig.Identity = WebRTCUtils::MakeParticipantIdentity(TEXT("sender"));
        TokenConfig.Role = EO3DTokenRole::Publisher;
        TokenConfig.RefreshLeadTimeSec = TokenSettings.RefreshLeadTimeSec;
        // Declared secret, resolved into Config.Secrets by the component or source (ADR 0004).
        TokenConfig.EndpointAuth = WebRTCUtils::FindSecret(Config.Secrets, WebRTCUtils::TokenEndpointAuthOptionKey);

        if (TokenConfig.EndpointUrl.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCSender, Error, TEXT("Auto-fetch enabled but no token endpoint URL provided"));
            return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("WebRTC sender: token auto-fetch is enabled but no token endpoint URL is set."));
        }

        if (TokenConfig.RoomName.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCSender, Error, TEXT("Auto-fetch enabled but no room set (transport option '%s')"), WebRTCUtils::RoomOptionKey);
            return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig,
                FString::Printf(TEXT("WebRTC sender: token auto-fetch is enabled but no room is set (transport option '%s')."), WebRTCUtils::RoomOptionKey));
        }

        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Token auto-fetch enabled: endpoint=%s, room=%s, identity=%s"),
            *O3DRedact::Url(TokenConfig.EndpointUrl), *TokenConfig.RoomName, *TokenConfig.Identity);
    }
    else
    {
        TokenConfig.Mode = EO3DTokenMode::Manual;
        TokenConfig.ManualToken = TokenSettings.ManualToken;

        if (TokenConfig.ManualToken.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCSender, Verbose, TEXT("WebRTC token not provided"));
            return FO3DTransportResult::Error(EO3DTransportError::AuthFailed, TEXT("WebRTC sender: no LiveKit token (manual token mode)."));
        }

        UE_LOG(LogO3DWebRTCSender, Log, TEXT("Manual token mode"));
    }

    if (!TokenManager->Initialize(TokenConfig))
    {
        UE_LOG(LogO3DWebRTCSender, Error, TEXT("Failed to initialize token manager"));
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("WebRTC sender: the token settings were refused."));
    }

    bPreferLossyData = O3DTransportOptions::GetBool(Config.AdvancedParams, WebRTCUtils::PreferLossyOptionKey, /*Default=*/false);
    bLossyCapabilities.store(bPreferLossyData);

    return FO3DTransportResult::Ok();
}

#endif // O3D_WITH_TRANSPORT_WEBRTC
