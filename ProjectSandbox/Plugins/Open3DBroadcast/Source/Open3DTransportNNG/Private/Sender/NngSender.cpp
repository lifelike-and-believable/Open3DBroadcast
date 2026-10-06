// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_NNG // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Sender/NngSender.h"
#include "O3DRedact.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "O3DFfiContextRegistry.h"
#include "O3DSinkAudioEncoder.h"
#include "O3DUnifiedMessage.h"
#include "O3DPerformanceMetrics.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

#if !defined(NNG_STATIC_LIB)
#define NNG_STATIC_LIB 1
#endif

THIRD_PARTY_INCLUDES_START
#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/protocol/pipeline0/push.h>
#include <nng/protocol/pubsub0/pub.h>
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogO3DNngSender, Log, All);

namespace O3DNngSenderPrivate
{
    constexpr uint64 MinQueueBytes = 64ull * 1024ull;
    constexpr uint64 MaxQueueBytes = 512ull * 1024ull * 1024ull;
    /** Idle wait of the worker; an Enqueue wakes it earlier. Also the pipe-state reporting delay. */
    constexpr uint32 IdleWaitMs = 50;
    constexpr uint32 PausedWaitMs = 5;

    /** Reopening a socket that failed to open or was closed: 0.1 s doubling to 5 s, as before WP-A1 PR 4d. */
    FO3DReconnectPolicySettings MakeReopenSettings()
    {
        FO3DReconnectPolicySettings Settings;
        Settings.InitialDelaySeconds = 0.1;
        Settings.MaxDelaySeconds = 5.0;
        Settings.Multiplier = 2.0;
        return Settings;
    }

    TO3DFfiContextRegistry<FNngSenderPipeContext>& GetSenderPipeContextRegistry()
    {
        static TO3DFfiContextRegistry<FNngSenderPipeContext> Registry;
        return Registry;
    }

    /** NNG pipe callback. `Context` is an opaque token, never a sender pointer (WP-S5). */
    void SenderPipeCallback(nng_pipe /*Pipe*/, nng_pipe_ev Event, void* Context)
    {
        const TSharedPtr<FNngSenderPipeContext, ESPMode::ThreadSafe> Pipe = GetSenderPipeContextRegistry().Resolve(Context);
        if (!Pipe.IsValid())
        {
            return;
        }

        if (Event == NNG_PIPE_EV_ADD_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_add(1) + 1;
            Pipe->bConnected.store(true);
            UE_LOG(LogO3DNngSender, Log, TEXT("NNG sender connection established (pipe count=%d)"), Count);

            // ADR 0005 (vi): the new peer has no full Subject yet. Copied under the lock and
            // called outside it.
            FO3DPeerJoinedCallback Callback;
            {
                FScopeLock Guard(&Pipe->PeerJoinedLock);
                Callback = Pipe->PeerJoined;
            }
            if (Callback)
            {
                Callback();
            }
        }
        else if (Event == NNG_PIPE_EV_REM_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_sub(1) - 1;
            if (Count <= 0)
            {
                Pipe->bConnected.store(Pipe->bConnectedWithoutPipes.load());
            }
            UE_LOG(LogO3DNngSender, Log, TEXT("NNG sender connection lost (pipe count=%d)"), FMath::Max(0, Count));
        }
    }
}

struct FO3DNngSender::FNngSocketWrapper
{
    nng_socket Socket{ NNG_SOCKET_INITIALIZER };

    ~FNngSocketWrapper()
    {
        if (Socket.id != 0)
        {
            nng_close(Socket);
            Socket.id = 0;
        }
    }
};

FO3DNngSender::FO3DNngSender()
    : Queue(MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>())
    , PublishState(MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue, EO3DAudioWireFormat::UnifiedEnvelope))
    , ReopenPolicy(O3DNngSenderPrivate::MakeReopenSettings())
    , PipeContext(MakeShared<FNngSenderPipeContext, ESPMode::ThreadSafe>())
    , Context(FO3DRuntimeContext::Default())
    , TransportMetrics(Context->GetMetrics().AcquireTransportMetrics(TEXT("NNG")))
    , SenderMetrics(Context->GetMetrics().AcquireSenderMetrics(TEXT("NNG sender")))
{
    PipeToken = O3DNngSenderPrivate::GetSenderPipeContextRegistry().Register(PipeContext);
}

void FO3DNngSender::SetPeerJoinedCallback(FO3DPeerJoinedCallback Callback)
{
    FScopeLock Guard(&PipeContext->PeerJoinedLock);
    PipeContext->PeerJoined = MoveTemp(Callback);
}

void FO3DNngSender::SetFramesDroppedCallback(FO3DFramesDroppedCallback Callback)
{
    FScopeLock Guard(&FramesDroppedLock);
    FramesDroppedCallback = MoveTemp(Callback);
}

void FO3DNngSender::NotifyFramesDropped()
{
    FO3DFramesDroppedCallback Callback;
    {
        FScopeLock Guard(&FramesDroppedLock);
        Callback = FramesDroppedCallback;
    }
    if (Callback)
    {
        Callback();
    }
}

FO3DTransportResult FO3DNngSender::Initialize(const FO3DTransportConfig& Config)
{
    // The worker reads Options and owns the socket; neither may change under it (TRB-33).
    Stop();

    FString Error;
    if (!O3DNNG::ParseSenderOptions(Config, Options, Error))
    {
        UE_LOG(LogO3DNngSender, Warning, TEXT("Failed to parse NNG sender config: %s"), *Error);
        bInitialized = false;
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("NNG sender config: %s"), *Error));
    }

    Options.MaxQueueBytes = FMath::Clamp<uint64>(Options.MaxQueueBytes, O3DNngSenderPrivate::MinQueueBytes, O3DNngSenderPrivate::MaxQueueBytes);
    UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender queue limit set to %llu bytes"), Options.MaxQueueBytes);
    CapabilityMode.store(Options.Mode);
    QueueLimitBytes.store(Options.MaxQueueBytes);

    // nng.qmax is the byte limit of frames and, separately, of audio, so neither can take the
    // other's room; control keeps the queue's own cap (ADR 0007 item 7, ADR 0011). RefuseNewest:
    // the queue never discards what it accepted (see the class comment).
    FO3DSendQueueLimits Limits;
    Limits.Mocap.MaxBytes = static_cast<int64>(Options.MaxQueueBytes);
    Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
    Limits.Audio.MaxBytes = static_cast<int64>(Options.MaxQueueBytes);
    Queue->SetLimits(Limits);

    ActiveConfig = Config;
    ActiveAudioConfig = Config.Audio;
    Context = FO3DRuntimeContext::OrDefault(Config.Context);
    TransportMetrics = Context->GetMetrics().AcquireTransportMetrics(TEXT("NNG"));
    if (!Context->ResolveSenderMetrics(Config.SenderMetrics, TEXT("NNG sender"), SenderMetrics))
    {
        bInitialized = false;
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("NNG sender: the sender metrics handle belongs to another runtime context."));
    }
    // Note: Audio stream label is now automatically derived from StreamId
    AudioSourceGuid = FGuid::NewGuid();

    FramesSent.store(0);
    BytesSent.store(0);
    DroppedFrames.store(0);
    SendErrors.store(0);
    PipeContext->PipeCount.store(0);
    // A listening socket is ready without peers; a dialing one only once a pipe exists.
    PipeContext->bConnectedWithoutPipes.store(Options.bListen);
    LastErrorLogTimestamp = 0.0;
    LastDropLogTimestamp = 0.0;
    DropsSinceLastLog = 0;
    LastBackpressureLogTimestamp.store(0.0);
    PublishState->GetSubjectSlot().Reset();

    bInitialized = true;
    PublishState->Open();
    return FO3DTransportResult::Ok();
}

FO3DNngSender::~FO3DNngSender()
{
    Stop();
    // nng_close() in Stop() has returned, so no pipe callback is running for this socket
    // (needs-FFI-verification); a late one would resolve the token to nothing anyway.
    O3DNngSenderPrivate::GetSenderPipeContextRegistry().Unregister(PipeToken);
    PipeToken = nullptr;
}

FO3DTransportResult FO3DNngSender::Start()
{
    if (!bInitialized.load())
    {
        UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender Start called before Initialize"));
        return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("NNG sender Start() before a successful Initialize()."));
    }

    if (bRunning.load())
    {
        return FO3DTransportResult::Ok();
    }

    // The worker does not exist yet, so this thread owns the socket and the queue here (TRB-33).
    DrainQueue();
    ReopenPolicy = FO3DReconnectPolicy(O3DNngSenderPrivate::MakeReopenSettings());
    int32 OpenError = 0;
    const bool bOpened = OpenSocket(&OpenError);
    if (!bOpened && Options.bListen)
    {
        // OpenSocket logged the reason (for example, the port is in use). A listener must report
        // this at once (ADR 0007 item 3), so it is opened here rather than on the worker.
        const FO3DTransportResult Result = FO3DTransportResult::Error(
            OpenError == NNG_EADDRINUSE ? EO3DTransportError::AddressInUse : EO3DTransportError::ConnectFailed,
            FString::Printf(TEXT("NNG sender could not listen on %s (%d %s)."), *O3DRedact::Url(Options.CanonicalUri), OpenError,
                OpenError != 0 ? UTF8_TO_TCHAR(nng_strerror(OpenError)) : TEXT("")));
        ConnectionState.End(EO3DConnectionState::Failed, Result);
        return Result;
    }
    if (!bOpened)
    {
        ReopenPolicy.OnFailure(FPlatformTime::Seconds());
    }

    PublishState->Open();

    // No peer pipe yet; the worker reports the first one. A dialer that could not dial yet
    // keeps retrying (below), so it is Connecting too.
    bWorkerSawPeer = false;
    ConnectionState.Begin(EO3DConnectionState::Connecting);

    if (!Worker.Start(TEXT("O3D_NNG_Sender_Worker"), [this]() { return RunWorkerIteration(); }, Queue))
    {
        UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender could not start its worker thread."));
        CloseSocket();
        const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("NNG sender could not start its worker thread."));
        ConnectionState.End(EO3DConnectionState::Failed, Result);
        return Result;
    }
    bRunning = true;

    UE_LOG(LogO3DNngSender, Log, TEXT("NNG sender started - Mode=%s Role=%s URI=%s (queue=%llu bytes)"),
        *O3DNNG::ModeToString(Options.Mode),
        *O3DNNG::RoleToString(Options.Role),
        *O3DRedact::Url(Options.CanonicalUri),
        Options.MaxQueueBytes);

    if (!bOpened)
    {
        UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender could not dial %s yet; retrying with backoff (check host/port)"), *O3DRedact::Url(Options.CanonicalUri));
    }

    return FO3DTransportResult::Ok();
}

void FO3DNngSender::Stop()
{
    // WP-S5 ordering: close the audio gate (waits for in-flight submits), join the worker (so no
    // nng_send is in flight), close the socket, drain. The gate is closed even when not running so
    // a sink never outlives Stop().
    PublishState->Close();

    if (!bRunning.load())
    {
        ConnectionState.End(EO3DConnectionState::Idle);
        return;
    }

    bRunning = false;
    Worker.Stop();

    // The worker has exited, so this thread owns the socket again (TRB-33).
    CloseSocket();
    DrainQueue();

    PipeContext->bConnected.store(false);
    // The worker, the only thread that reports changes while running, has exited.
    ConnectionState.End(EO3DConnectionState::Idle);
    UE_LOG(LogO3DNngSender, Log, TEXT("NNG sender stopped"));
}

EO3DSendResult FO3DNngSender::SendSerialized(FO3DSendPayload&& Payload)
{
    if (!bInitialized.load() || !bRunning.load())
    {
        return EO3DSendResult::NotRunning;
    }

    if (Payload.Bytes.Num() <= 0)
    {
        return EO3DSendResult::Invalid;
    }

    // No NotConnected: a frame queued before a peer exists is dropped and counted by the worker,
    // as NNG itself would. The queue takes Payload.Bytes without a copy.
    return EnqueueFrame(MoveTemp(Payload.Bytes), MoveTemp(Payload.Subject), Payload.CaptureTimeSec, Payload.bFullSync);
}

EO3DSendResult FO3DNngSender::EnqueueFrame(TArray<uint8>&& Bytes, FString Subject, double CaptureTimeSec, bool bFullSync)
{
    // Audio frames carry the subject last sent (their metadata's SubjectName).
    if (!Subject.IsEmpty())
    {
        PublishState->GetSubjectSlot().Set(Subject);
    }

    const int32 Len = Bytes.Num();
    const EO3DSendResult Result = Queue->Enqueue(FO3DSendItem::MakeMocap(MoveTemp(Bytes), MoveTemp(Subject), CaptureTimeSec, bFullSync));
    if (Result != EO3DSendResult::Queued)
    {
        DroppedFrames.fetch_add(1);

        // Any sending thread may get here; the compare-exchange lets one of them log (TRB-43).
        const double Now = FPlatformTime::Seconds();
        double Last = LastBackpressureLogTimestamp.load();
        if (Now - Last > 2.0 && LastBackpressureLogTimestamp.compare_exchange_strong(Last, Now))
        {
            UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender queue full (pending=%lld / limit=%llu bytes). Dropping frame."),
                Queue->GetStats().Mocap.PendingBytes, QueueLimitBytes.load());
        }
        return Result;
    }

    return EO3DSendResult::Queued;
}

/**
 * Control (ADR 0011): the envelope rides the send queue in-band, as audio does, with a cap of its
 * own, and the worker sends it in queue order. Not counted as a frame. Pair and push sockets
 * deliver it reliably; a pub socket can drop it for a slow subscriber, which the control
 * publisher's redundancy and snapshots cover (ADR 0005 Q3).
 */
EO3DSendResult FO3DNngSender::SendControl(const uint8* Envelope, int32 Len)
{
    if (!bInitialized.load() || !bRunning.load())
    {
        return EO3DSendResult::NotRunning;
    }
    TConstArrayView<uint8> Payload;
    if (!O3DS::TryGetControlPayload(Envelope, Len, Payload))
    {
        return EO3DSendResult::Invalid;
    }
    return Queue->Enqueue(FO3DSendItem::MakeControl(TArray<uint8>(Envelope, Len)));
}

void FO3DNngSender::Tick(float /*DeltaSeconds*/)
{
    // Nothing to do on the game thread: the worker owns the socket and reopens it (TRB-33).
    // A dialer also reconnects by itself inside NNG after a dropped connection.
}

FO3DTransportStats FO3DNngSender::GetStats() const
{
    const FO3DSendQueueStats QueueStats = Queue->GetStats();
    FO3DTransportStats Copy;
    Copy.FramesSent = FramesSent.load();
    Copy.BytesSent = BytesSent.load();
    Copy.DroppedFrames = DroppedFrames.load();
    Copy.SendErrors = SendErrors.load();
    Copy.PendingFrames = QueueStats.Mocap.PendingItems;
    Copy.PendingBytes = QueueStats.GetPendingBytes();
    Copy.State = ConnectionState.Get();
    return Copy;
}

bool FO3DNngSender::OpenSocket(int32* OutNngError)
{
    CloseSocket();
    if (OutNngError)
    {
        *OutNngError = 0;
    }

    FNngSocketWrapper* NewSocket = new FNngSocketWrapper();
    int Ret = 0;

    switch (Options.Mode)
    {
    case O3DNNG::ENngMode::Pub:
        Ret = nng_pub0_open(&NewSocket->Socket);
        break;
    case O3DNNG::ENngMode::Pair:
        Ret = nng_pair1_open(&NewSocket->Socket);
        break;
    case O3DNNG::ENngMode::Push:
        Ret = nng_push0_open(&NewSocket->Socket);
        break;
    default:
        UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender unsupported mode %s"), *O3DNNG::ModeToString(Options.Mode));
        delete NewSocket;
        return false;
    }

    if (Ret == 0)
    {
        // Options and pipe notifications are set before listen/dial, so the first pipe event
        // cannot be missed and "connected" comes only from pipe events for a dialer (TRB-42).
        PipeContext->PipeCount.store(0);
        PipeContext->bConnected.store(false);

        const int NotifyAdd = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_ADD_POST, O3DNngSenderPrivate::SenderPipeCallback, PipeToken);
        if (NotifyAdd != 0)
        {
            UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender pipe notify add failed (%d) %s"), NotifyAdd, UTF8_TO_TCHAR(nng_strerror(NotifyAdd)));
        }
        const int NotifyRem = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_REM_POST, O3DNngSenderPrivate::SenderPipeCallback, PipeToken);
        if (NotifyRem != 0)
        {
            UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender pipe notify remove failed (%d) %s"), NotifyRem, UTF8_TO_TCHAR(nng_strerror(NotifyRem)));
        }

        // NNG's send buffer is an int counting messages (0-8192), not bytes (TRB-36). With the
        // default depth, a pub socket drops whatever overflows a subscriber's per-pipe queue, so a
        // burst of frames loses messages even on 127.0.0.1. The application queue (nng.qmax)
        // remains the byte limit and backpressure point.
        constexpr int NngSendBufMessages = 1024;
        const int SetSendBufRet = nng_socket_set_int(NewSocket->Socket, NNG_OPT_SENDBUF, NngSendBufMessages);
        if (SetSendBufRet != 0)
        {
            UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender could not set send buffer to %d messages (%d %s)"),
                NngSendBufMessages, SetSendBufRet, UTF8_TO_TCHAR(nng_strerror(SetSendBufRet)));
        }

        // No NNG_OPT_SENDTIMEO (TRB-36): every nng_send below uses NNG_FLAG_NONBLOCK, which
        // returns NNG_EAGAIN at once instead of waiting, so a send timeout would never apply.

        const FTCHARToUTF8 AddressUtf8(*Options.TcpAddress);
        if (Options.bListen)
        {
            Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
        }
        else
        {
            // Non-blocking dial: NNG keeps retrying in the background and reconnects a dropped
            // connection by itself, so the worker only reopens the socket if this call fails.
            Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
        }
    }

    if (Ret != 0)
    {
        UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender could not %s %s (%d) %s"),
            Options.bListen ? TEXT("listen on") : TEXT("dial"),
            *O3DRedact::Url(Options.CanonicalUri), Ret, UTF8_TO_TCHAR(nng_strerror(Ret)));
        if (OutNngError)
        {
            *OutNngError = Ret;
        }
        delete NewSocket;
        Socket = nullptr;
        return false;
    }

    if (Options.bListen)
    {
        // A listener is ready to accept peers; pipe events only track how many there are.
        PipeContext->bConnected.store(true);
    }

    Socket = NewSocket;
    return true;
}

void FO3DNngSender::CloseSocket()
{
    if (Socket)
    {
        // nng_close: only the thread that owns the socket gets here, so no nng_send is in flight.
        delete Socket;
        Socket = nullptr;
    }
}

bool FO3DNngSender::EnsureSocketOnWorker()
{
    if (Socket)
    {
        return true;
    }

    const double Now = FPlatformTime::Seconds();
    if (!ReopenPolicy.IsDue(Now))
    {
        return false;
    }
    if (OpenSocket())
    {
        ReopenPolicy.OnSuccess();
        return true;
    }
    ReopenPolicy.OnFailure(Now);
    return false;
}

uint32 FO3DNngSender::RunWorkerIteration()
{
    using namespace O3DNngSenderPrivate;

    if (bWorkerPausedForTesting.load())
    {
        PausedIterations.fetch_add(1);
        return PausedWaitMs;
    }

    // The worker is the only thread that opens, closes or reopens the socket while running (TRB-33).
    EnsureSocketOnWorker();
    UpdateConnectionStateOnWorker();

    FO3DSendItem Item;
    const bool bDequeued = Queue->Dequeue(Item);
    if (const int32 Discarded = Queue->ConsumeMocapDiscarded())
    {
        for (int32 Index = 0; Index < Discarded; ++Index)
        {
            SenderMetrics->RecordTransportFrameDropped(); // WP-R3
        }
        NotifyFramesDropped(); // WP-R1 (TR-1): evicted after it was accepted
    }
    if (!bDequeued)
    {
        return IdleWaitMs;
    }

    const bool bFrame = Item.Kind == EO3DSendItemKind::Mocap;
    if (!Socket)
    {
        if (bFrame)
        {
            RecordSendDrop();
        }
        return 0;
    }

    const int32 PayloadSize = Item.Bytes.Num();
    const int Ret = nng_send(Socket->Socket, Item.Bytes.GetData(), static_cast<size_t>(PayloadSize), NNG_FLAG_NONBLOCK);
    if (Ret == NNG_EAGAIN)
    {
        // No peer ready, or NNG's own send buffer is full (TRB-34). This item is the oldest one
        // queued: drop it. It is never put back at the tail, which would reorder frames,
        // busy-spin while no peer exists and replay a stale backlog. Frames are counted.
        if (bFrame)
        {
            RecordSendDrop();
        }
        return 0;
    }
    if (Ret != 0)
    {
        HandleSendError(Ret, bFrame);
        return 0;
    }

    // Counted here, once nng_send took the message (the conformance round trip waits for it, #301).
    if (bFrame)
    {
        FramesSent.fetch_add(1);
        BytesSent.fetch_add(PayloadSize);
        // WP-R3: sent, not queued.
        SenderMetrics->RecordBytesSent(static_cast<uint64>(PayloadSize));
        TransportMetrics->RecordFrameSent(static_cast<uint64>(PayloadSize));
    }
    else if (Item.Kind == EO3DSendItemKind::Audio)
    {
        BytesSent.fetch_add(PayloadSize);
    }
    return 0;
}

void FO3DNngSender::UpdateConnectionStateOnWorker()
{
    // Pipe events arrive on NNG threads that hold only the pipe context, so the worker turns
    // the pipe count into connection-state changes (ADR 0007 item 3).
    const bool bHasPeer = Socket != nullptr && PipeContext->PipeCount.load() > 0;
    if (bHasPeer == bWorkerSawPeer)
    {
        return;
    }
    bWorkerSawPeer = bHasPeer;
    if (bHasPeer)
    {
        ConnectionState.Set(EO3DConnectionState::Connected);
    }
    else
    {
        ConnectionState.Set(EO3DConnectionState::Reconnecting,
            FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, TEXT("The last NNG peer disconnected.")));
    }
}

void FO3DNngSender::SetWorkerPausedForTesting(bool bPaused)
{
    const int64 Before = PausedIterations.load();
    bWorkerPausedForTesting.store(bPaused);
    Queue->Wake();
    if (!bPaused)
    {
        return;
    }
    // An iteration that started before the store may still dequeue; the next one sees the flag.
    // Wait for that one, so a test's sends after this call stay queued.
    const double Deadline = FPlatformTime::Seconds() + 5.0;
    while (Worker.IsRunning() && PausedIterations.load() == Before && FPlatformTime::Seconds() < Deadline)
    {
        FPlatformProcess::YieldThread();
    }
}

void FO3DNngSender::RecordSendDrop()
{
    DroppedFrames.fetch_add(1);
    SenderMetrics->RecordTransportFrameDropped();
    // WP-R1 (TR-1): the frame was accepted by SendSerialized; pair and push are ReliableOrdered,
    // so residual receivers need a full sync after the gap.
    NotifyFramesDropped();

    // Dropping while no peer is connected is expected (a dialer before its first connection),
    // so this is a rate-limited Log line, not a warning.
    ++DropsSinceLastLog;
    const double Now = FPlatformTime::Seconds();
    if (Now - LastDropLogTimestamp > 2.0)
    {
        UE_LOG(LogO3DNngSender, Log, TEXT("NNG sender dropped %lld frame(s): no peer ready or NNG send buffer full (%s)"),
            DropsSinceLastLog, *O3DRedact::Url(Options.CanonicalUri));
        LastDropLogTimestamp = Now;
        DropsSinceLastLog = 0;
    }
}

void FO3DNngSender::DrainQueue()
{
    // Only while the worker is not running: Empty() is a consumer-side call. Not counted as drops.
    check(!Worker.IsRunning());
    Queue->Empty();
}

void FO3DNngSender::HandleSendError(int ErrorCode, bool bFrame)
{
    SendErrors.fetch_add(1);
    if (bFrame)
    {
        DroppedFrames.fetch_add(1);
    }

    const double Now = FPlatformTime::Seconds();
    if (Now - LastErrorLogTimestamp > 2.0)
    {
        UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender send failed (%d) %s"), ErrorCode, UTF8_TO_TCHAR(nng_strerror(ErrorCode)));
        LastErrorLogTimestamp = Now;
    }

    if (ErrorCode == NNG_ECLOSED)
    {
        // The socket is unusable. Close it here, on the worker, and let EnsureSocketOnWorker
        // reopen it when the reopen policy allows.
        CloseSocket();
        PipeContext->bConnected.store(false);
        ReopenPolicy.OnFailure(Now);
    }
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DNngSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
    FO3DTransportAudioConfig EffectiveConfig = ActiveAudioConfig;
    if (AudioConfig.bEnableAudio)
    {
        EffectiveConfig = AudioConfig;
    }

    EffectiveConfig.bEnableAudio = true;
    EffectiveConfig.NumChannels = FMath::Max(EffectiveConfig.NumChannels, 1);
    EffectiveConfig.SampleRate = FMath::Max(EffectiveConfig.SampleRate, 1);
    // Note: Audio stream label is now automatically derived from StreamId

    ActiveAudioConfig = EffectiveConfig;

    // Immutable snapshot for this sink's own encoders (TRB-11): nothing reconfigures them later.
    const FString SubjectFallback = ResolveAudioSubjectFallback();
    FO3DSinkAudioEncoder::FSettings EncoderSettings;
    EncoderSettings.Config = EffectiveConfig;
    EncoderSettings.DefaultStreamLabel = SubjectFallback;
    EncoderSettings.DefaultSubject = SubjectFallback;
    EncoderSettings.SourceGuid = AudioSourceGuid;

    return MakeShared<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe>(PublishState, EffectiveConfig, MoveTemp(EncoderSettings));
}

FString FO3DNngSender::ResolveAudioSubjectFallback() const
{
    FString SubjectFallback = ActiveConfig.StreamId;
    if (SubjectFallback.IsEmpty())
    {
        SubjectFallback = Options.StreamId;
    }
    if (SubjectFallback.IsEmpty())
    {
        SubjectFallback = TEXT("nng");
    }
    return SubjectFallback;
}

#endif // O3D_WITH_TRANSPORT_NNG
