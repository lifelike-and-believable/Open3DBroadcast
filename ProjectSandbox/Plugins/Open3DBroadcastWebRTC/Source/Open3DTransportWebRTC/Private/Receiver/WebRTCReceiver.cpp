// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

#include "WebRTCReceiver.h"
#include "O3DLogThrottle.h"
#include "O3DRedact.h"
#include "../Shared/WebRTCUtils.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Math/NumericLimits.h"
#include "Containers/StringConv.h"
#include "O3DFfiContextRegistry.h"
#include "O3DUnifiedMessage.h"

namespace
{
	// WP-R3 (TR-6): one throttle per hot-path log site (the peer or the frame rate decides how often these fire).
	FO3DLogThrottle WebRTCOversizePayloadLog;
}


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

    /** Most control envelopes waiting for Poll(); the publisher's snapshots repair any dropped. */
    static constexpr int32 MaxPendingControlEnvelopes = 1024;

    /** Seconds between control drop logs (per receiver). */
    static constexpr double ControlDropLogIntervalSec = 2.0;

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
            int64 Suppressed = 0;
            if (WebRTCOversizePayloadLog.ShouldLog(Suppressed))
            {
                UE_LOG(LogO3DWebRTCReceiver, Error,
                    TEXT("Rejecting oversized WebRTC data payload len=%zu (max=%zu); %lld similar since the last error"),
                    Len, MaxIncomingDataPayloadBytes, Suppressed);
            }
            return false;
        }
        return true;
    }
}

FWebRTCReceiverLink::FWebRTCReceiverLink()
{
    // Frames: a byte budget, the newest refused when it is full (they used to queue without a
    // limit). Control: its own queue with the cap the receiver always had.
    FO3DSendQueueLimits FrameLimits;
    FrameLimits.Mocap.MaxBytes = MaxPendingFrameBytes;
    FrameLimits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
    FrameQueue.SetLimits(FrameLimits);

    FO3DSendQueueLimits ControlLimits;
    ControlLimits.Control.MaxItems = MaxPendingControlEnvelopes;
    ControlQueue.SetLimits(ControlLimits);
}

void FWebRTCReceiverLink::EnqueueFrame(const FString& SubjectLabel, const uint8* Bytes, int32 Len)
{
    const double Now = FPlatformTime::Seconds();
    // The receive time rides in CaptureTimeSec; Poll measures the hand-off latency from it.
    if (FrameQueue.Enqueue(FO3DSendItem::MakeMocap(TArray<uint8>(Bytes, Len), SubjectLabel, Now)) != EO3DSendResult::Queued)
    {
        FramesRefused.fetch_add(1);
    }

    LastDataReceiveTime.store(Now);
    bReconnectPending.Store(false);
}

bool FWebRTCReceiverLink::ConsumeControl(const uint8* Bytes, size_t Len)
{
    if (!WebRTCUtils::IsControlKindEnvelope(Bytes, Len))
    {
        return false;
    }

    // Control traffic shows the link is alive (a control-only sender has no mocap), but it is not
    // a frame: no frame or byte counter moves.
    LastDataReceiveTime.store(FPlatformTime::Seconds());
    bReconnectPending.Store(false);

    if (!bControlWanted.Load())
    {
        LogControlDrop(TEXT("no control sink"));
        return true;
    }

    // Len was bounded by IsAcceptablePayload, so the cast is safe. Only the envelope itself is
    // kept: header plus payload, never trailing bytes.
    TConstArrayView<uint8> Payload;
    if (!O3DS::TryGetControlPayload(Bytes, static_cast<int32>(Len), Payload))
    {
        LogControlDrop(TEXT("malformed control envelope"));
        return true;
    }
    // The envelope ends where its payload does (v1 and v2 headers differ in size).
    const int32 EnvelopeBytes = static_cast<int32>(Payload.GetData() - Bytes) + Payload.Num();

    if (ControlQueue.Enqueue(FO3DSendItem::MakeControl(TArray<uint8>(Bytes, EnvelopeBytes))) != EO3DSendResult::Queued)
    {
        LogControlDrop(TEXT("control queue full"));
    }
    return true;
}

void FWebRTCReceiverLink::LogControlDrop(const TCHAR* Reason)
{
    const double Now = FPlatformTime::Seconds();
    double Last = LastControlDropLogTime.load();
    if (Now - Last >= ControlDropLogIntervalSec && LastControlDropLogTime.compare_exchange_strong(Last, Now))
    {
        UE_LOG(LogO3DWebRTCReceiver, Verbose, TEXT("WebRTC control envelope dropped (%s)"), Reason);
    }
}

void FWebRTCReceiverLink::RequestReconnect()
{
    if (!bReconnectPending.Exchange(true))
    {
        // Log, not Warning: the connection loss that leads here is already a Warning (TRF-30).
        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver scheduling reconnect"));
    }
}

// Static callback for connection state changes
void FO3DWebRTCReceiver::OnConnectionState(void* user, LkConnectionState state, int32_t reason_code, const char* message)
{
    const TSharedPtr<FWebRTCReceiverLink, ESPMode::ThreadSafe> Self = GetReceiverLinkRegistry().Resolve(user);
    if (!Self.IsValid()) return;

    // Read by Poll on the game thread (ADR 0007 item 3); the callback never reaches the receiver.
    Self->LkReasonCode.store(static_cast<int32>(reason_code));
    Self->LkState.store(static_cast<int32>(state));

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

    // Control first (ADR 0011 item 7): enveloped control bytes never reach the "label = subject"
    // mocap path, whatever their label. Plain bytes on the `__o3d.ctl` label stay mocap.
    if (Self->ConsumeControl(bytes, len))
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

// Unlabeled data callback: the only one livekit_ffi's async-connect loop calls. Frames take the
// 'default' label, which only logs see.
void FO3DWebRTCReceiver::OnDataReceived(void* user, const uint8_t* bytes, size_t len)
{
    const TSharedPtr<FWebRTCReceiverLink, ESPMode::ThreadSafe> Self = GetReceiverLinkRegistry().Resolve(user);
    if (!Self.IsValid() || !IsAcceptablePayload(bytes, len))
    {
        return;
    }

    if (Self->ConsumeControl(bytes, len))
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

FO3DTransportResult FO3DWebRTCReceiver::Initialize(const FO3DTransportConfig& Config)
{
    FScopeLock Lock(&StateMutex);

    if (bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCReceiver, Warning, TEXT("WebRTC receiver already initialized"));
        return FO3DTransportResult::Error(EO3DTransportError::Internal, TEXT("WebRTC receiver is already initialized; Stop() it first."));
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
    return FO3DTransportResult::Error(EO3DTransportError::Unsupported, TEXT("The WebRTC transport needs Windows 64-bit."));
#endif

    if (!Ffi.IsComplete())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver: LiveKit function table is incomplete"));
        return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("The LiveKit library is not loaded or is incomplete."));
    }

    Link->bConnected.Store(false);

    const FO3DTransportResult ConfigResult = ParseConfig(Config);
    if (!ConfigResult.IsOk())
    {
        return ConfigResult;
    }

    ActiveAudioConfig = Config.Audio;

    if (!SetupClientHandle())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC receiver: failed to set up the LiveKit client"));
        return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("Failed to set up the LiveKit client."));
    }

    ActiveConfig = Config;
    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.Reset();
        LatencySamples = 0;
    }
    // Control is delivered under the receiver's stream id (ADR 0011).
    FO3DReceiveDemuxSettings DemuxSettings = Demux.GetSettings();
    DemuxSettings.StreamId = Config.StreamId;
    Demux.SetSettings(DemuxSettings);
    Demux.ResetStats();
    Link->FramesRefused.store(0);
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

    return FO3DTransportResult::Ok();
}

void FO3DWebRTCReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
    FScopeLock Lock(&StateMutex);
    Demux.SetConsumer(InConsumer);
}

FO3DTransportResult FO3DWebRTCReceiver::Start()
{
    FScopeLock Lock(&StateMutex);

    if (!bInitialized.Load())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Cannot start WebRTC receiver: not initialized"));
        return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("WebRTC receiver Start() before a successful Initialize()."));
    }

    // ADR 0007 item 3: a receiver without a consumer refuses to start (it used to start and drop
    // every frame; FSerializedFrameConsumerRegistry, its old fallback, is gone, SHR-24).
    if (!Demux.HasConsumer())
    {
        return FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("WebRTC receiver Start() without a frame consumer (SetConsumer)."));
    }

    if (!SetupClientHandle())
    {
        const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("Failed to set up the LiveKit client."));
        ConnectionState.End(EO3DConnectionState::Failed, Result);
        return Result;
    }

    // "Connect requested" is tracked separately from the token (TRF-3). Without a token yet,
    // Poll() connects as soon as the token manager has one.
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
    return FO3DTransportResult::Ok();
}

void FO3DWebRTCReceiver::Stop()
{
    FScopeLock Lock(&StateMutex);

    bConnectRequested = false;
    bConnectIssued = false;
    // Poll applies LiveKit states only while connecting is requested, and the tracker ignores
    // changes once its session ended, so no state callback runs after Stop.
    ConnectionState.End(EO3DConnectionState::Idle);

    // Cancel any token fetch; its result is dropped (TRF-24).
    if (TokenManager.IsValid())
    {
        TokenManager->Reset();
    }

    DestroyClientHandle(TEXT("LiveKit disconnect"));

    // The consumer and the control sink are held strongly until here (TRF-38, ADR 0011 item 6).
    Demux.ReleaseSinks();
    Link->bControlWanted.Store(false);
    // lk_client_destroy has returned, so no data callback is filling the queues; this thread is
    // their only consumer.
    Link->ControlQueue.Empty();
    {
        // lk_disconnect/lk_client_destroy above have returned, so no callback is running
        // (livekit_ffi.h: "After lk_disconnect() or lk_client_destroy() returns, no further
        // callbacks will be invoked"). A late callback would resolve Link through the registry,
        // which is still safe.
        FScopeLock SinkLock(&Link->AudioSinkMutex);
        Link->AudioSink.Reset();
    }
    Link->FrameQueue.Empty();

    Link->bConnected.Store(false);
    Link->bPendingAudioFormatApply.Store(false);
    Link->bReconnectPending.Store(false);
    bInitialized.Store(false);

    UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("WebRTC receiver stopped"));
}

int32 FO3DWebRTCReceiver::Poll()
{
    // Game thread: the only consumer of the Link queues and the only user of the demux.
    int32 FramesProcessed = 0;
    int64 DroppedWithoutConsumer = 0;
    const double NowSeconds = FPlatformTime::Seconds();
    const bool bHaveConsumer = Demux.HasConsumer();

    // Every queued frame, in arrival order (so in order per subject).
    FO3DSendItem Item;
    while (Link->FrameQueue.Dequeue(Item))
    {
        if (!bHaveConsumer)
        {
            ++DroppedWithoutConsumer;
            continue;
        }

        const double ReceiveLatencyMs = FMath::Max(0.0, (NowSeconds - Item.CaptureTimeSec) * 1000.0);
        Link->FramesReceived.IncrementExchange();
        Link->BytesReceived.AddExchange(Item.Bytes.Num());
        {
            FScopeLock Lock(&StatsMutex);
            Stats.MaxLatencyMs = FMath::Max(Stats.MaxLatencyMs, ReceiveLatencyMs);
            const int64 NewSampleCount = LatencySamples + 1;
            const double PreviousTotal = Stats.AverageLatencyMs * LatencySamples;
            Stats.AverageLatencyMs = (PreviousTotal + ReceiveLatencyMs) / FMath::Max<int64>(1, NewSampleCount);
            LatencySamples = NewSampleCount;
        }

        // The data label is the subject. LiveLink expects WorldTime to be "when to display", so
        // the frame is submitted with the current time rather than the arrival time.
        // The hand-off item owns the frame, so the consumer gets it without a copy (WP-A1 PR 5b).
        Demux.DeliverMocapOwned(Item.Subject, MoveTemp(Item.Bytes), FPlatformTime::Seconds());
        FramesProcessed++;
    }

    if (DroppedWithoutConsumer > 0)
    {
        UE_LOG(LogO3DWebRTCReceiver, Verbose, TEXT("Poll(): %lld frames dropped (no consumer)"), DroppedWithoutConsumer);
        FScopeLock Lock(&StatsMutex);
        Stats.DroppedFrames += DroppedWithoutConsumer;
    }

    // Control (ADR 0011): not a frame, so it is not counted in FramesProcessed or any frame stat.
    // Only well-formed envelopes were queued; the demux delivers them to the control sink.
    while (Link->ControlQueue.Dequeue(Item))
    {
        // Stamped with the delivery time, as before.
        Demux.DeliverControlEnvelope(Item.Bytes.GetData(), Item.Bytes.Num(), FPlatformTime::Seconds());
    }

    {
        FScopeLock Lock(&StateMutex);
        UpdateConnection();
        if (bConnectRequested)
        {
            UpdateConnectionState();
        }

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
    FO3DTransportStats Copy = Stats;
    Copy.DroppedFrames += Link->FramesRefused.load();
    // Frames waiting for Poll in the hand-off queue.
    Copy.PendingFrames = Link->FrameQueue.GetPendingItems(EO3DSendItemKind::Mocap);
    Copy.PendingBytes = Link->FrameQueue.GetPendingBytes();
    Copy.State = ConnectionState.Get();
    return Copy;
}

void FO3DWebRTCReceiver::UpdateConnectionState()
{
    // Game thread (Poll), StateMutex held. LiveKit reports on its own threads, which reach only
    // the shared link; the receiver reconnects by itself after a disconnect or failure.
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
    case static_cast<int32>(LkConnDisconnected):
    case static_cast<int32>(LkConnFailed):
        if (bEverConnected)
        {
            ConnectionState.Set(EO3DConnectionState::Reconnecting,
                FO3DTransportResult::Error(EO3DTransportError::ConnectFailed,
                    FString::Printf(TEXT("LiveKit connection lost (state=%d, code=%d); reconnecting."), LkState, Link->LkReasonCode.load())));
        }
        break;
    default:
        break;
    }
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

void FO3DWebRTCReceiver::SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink)
{
    FScopeLock Lock(&StateMutex);
    Demux.SetControlSink(Sink);
    Link->bControlWanted.Store(Sink.IsValid());
    if (!Sink.IsValid())
    {
        // Game thread, like Poll: the queue's only consumer.
        Link->ControlQueue.Empty();
    }
}

FO3DTransportResult FO3DWebRTCReceiver::ParseConfig(const FO3DTransportConfig& Config)
{
    const FString HostAddress = Config.Uri;
    if (HostAddress.IsEmpty())
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC host address not specified"));
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("WebRTC receiver: no LiveKit server address."));
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
        TokenConfig.Identity = WebRTCUtils::MakeParticipantIdentity(TEXT("receiver"));
        TokenConfig.Role = EO3DTokenRole::Subscriber;
        TokenConfig.RefreshLeadTimeSec = TokenSettings.RefreshLeadTimeSec;
        // Declared secret, resolved into Config.Secrets by the component or source (ADR 0004).
        TokenConfig.EndpointAuth = WebRTCUtils::FindSecret(Config.Secrets, WebRTCUtils::TokenEndpointAuthOptionKey);

        if (TokenConfig.EndpointUrl.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Auto-fetch enabled but no token endpoint URL provided"));
            return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("WebRTC receiver: token auto-fetch is enabled but no token endpoint URL is set."));
        }

        if (TokenConfig.RoomName.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Auto-fetch enabled but no room set (transport option '%s')"), WebRTCUtils::RoomOptionKey);
            return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig,
                FString::Printf(TEXT("WebRTC receiver: token auto-fetch is enabled but no room is set (transport option '%s')."), WebRTCUtils::RoomOptionKey));
        }

        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("Token auto-fetch enabled: endpoint=%s, room=%s, identity=%s"),
            *O3DRedact::Url(TokenConfig.EndpointUrl), *TokenConfig.RoomName, *TokenConfig.Identity);
    }
    else
    {
        TokenConfig.Mode = EO3DTokenMode::Manual;
        TokenConfig.ManualToken = TokenSettings.ManualToken;

        if (TokenConfig.ManualToken.IsEmpty())
        {
            UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("WebRTC token not provided"));
            return FO3DTransportResult::Error(EO3DTransportError::AuthFailed, TEXT("WebRTC receiver: no LiveKit token (manual token mode)."));
        }

        UE_LOG(LogO3DWebRTCReceiver, Log, TEXT("Manual token mode"));
    }

    if (!TokenManager->Initialize(TokenConfig))
    {
        UE_LOG(LogO3DWebRTCReceiver, Error, TEXT("Failed to initialize token manager"));
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("WebRTC receiver: the token settings were refused."));
    }

    // Strict number (O3DTransportOptions, WP-A1 PR 4f); anything else is the default.
    const double TimeoutSeconds = O3DTransportOptions::GetDouble(Config.AdvancedParams, ReconnectTimeoutOptionKey, DefaultNoDataReconnectTimeoutSec, 0.0, 300.0);
    NoDataReconnectTimeoutSec = TimeoutSeconds;

    return FO3DTransportResult::Ok();
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

    // Register both data callbacks. livekit_ffi's event loops never call both for one packet: the
    // sync-connect loop calls the labeled callback when set and the unlabeled one otherwise, and
    // the async-connect loop (lk_connect_with_role_async, used below) calls only the unlabeled
    // one (backend_livekit.rs, docs/livekit_ffi_feature_request.md section 13). With only the
    // labeled callback, as TRF-16 had it, this receiver got no data. The label is used for logs
    // only; streams are keyed by the subject names in each packet (RCV-5).
    LogIfFailed(Ffi, Ffi.lk_client_set_data_callback_ex(ClientHandle, FO3DWebRTCReceiver::OnDataReceivedEx, LinkToken),
        TEXT("LiveKit set extended data callback"));
    LogIfFailed(Ffi, Ffi.lk_client_set_data_callback(ClientHandle, FO3DWebRTCReceiver::OnDataReceived, LinkToken),
        TEXT("LiveKit set data callback"));

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

    // Game thread (Poll): frames of the old connection are discarded, pending control is kept.
    Link->FrameQueue.Empty();

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
