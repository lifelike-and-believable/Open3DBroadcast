// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_NNG // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Receiver/NngReceiver.h"
#include "O3DLogThrottle.h"
#include "O3DRedact.h"
#include "Transport/O3DTransportOptions.h"

#include "Logging/LogMacros.h"
#include "HAL/PlatformTime.h"
#include "O3DFfiContextRegistry.h"

#if !defined(NNG_STATIC_LIB)
#define NNG_STATIC_LIB 1
#endif

THIRD_PARTY_INCLUDES_START
#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/pubsub0/sub.h>
THIRD_PARTY_INCLUDES_END



DEFINE_LOG_CATEGORY_STATIC(LogO3DNngReceiver, Log, All);

namespace O3DNngReceiverPrivate
{
    /** Largest message accepted. Also set as NNG_OPT_RECVMAXSZ, so NNG enforces it (TRB-42). */
    constexpr uint64 MaxPayloadBytes = 50ull * 1024ull * 1024ull;

    /** Reopening a socket that failed to listen or dial: 0.1 s doubling to 5 s, as before WP-A1 PR 4d. */
    FO3DReconnectPolicySettings MakeReopenSettings()
    {
        FO3DReconnectPolicySettings Settings;
        Settings.InitialDelaySeconds = 0.1;
        Settings.MaxDelaySeconds = 5.0;
        Settings.Multiplier = 2.0;
        return Settings;
    }

    TO3DFfiContextRegistry<FNngReceiverPipeContext>& GetReceiverPipeContextRegistry()
    {
        static TO3DFfiContextRegistry<FNngReceiverPipeContext> Registry;
        return Registry;
    }

    /** NNG pipe callback. `Context` is an opaque token, never a receiver pointer (TRB-42). */
    void ReceiverPipeCallback(nng_pipe /*Pipe*/, nng_pipe_ev Event, void* Context)
    {
        const TSharedPtr<FNngReceiverPipeContext, ESPMode::ThreadSafe> Pipe = GetReceiverPipeContextRegistry().Resolve(Context);
        if (!Pipe.IsValid())
        {
            return;
        }

        if (Event == NNG_PIPE_EV_ADD_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_add(1) + 1;
            Pipe->bConnected.store(true);
            UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver pipe added (count=%d)"), Count);
        }
        else if (Event == NNG_PIPE_EV_REM_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_sub(1) - 1;
            if (Count <= 0)
            {
                Pipe->bConnected.store(Pipe->bConnectedWithoutPipes.load());
            }
            UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver pipe removed (count=%d)"), FMath::Max(0, Count));
        }
    }
}

struct FO3DNngReceiver::FNngSocketWrapper
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

FO3DNngReceiver::FO3DNngReceiver()
    : PipeContext(MakeShared<FNngReceiverPipeContext, ESPMode::ThreadSafe>())
    , ReopenPolicy(O3DNngReceiverPrivate::MakeReopenSettings())
{
    PipeToken = O3DNngReceiverPrivate::GetReceiverPipeContextRegistry().Register(PipeContext);
}

FO3DTransportResult FO3DNngReceiver::Initialize(const FO3DTransportConfig& Config)
{
    Stop();

    FString Error;
    if (!O3DNNG::ParseReceiverOptions(Config, Options, Error))
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("Failed to parse NNG receiver config: %s"), *Error);
        bInitialized = false;
        return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("NNG receiver config: %s"), *Error));
    }

    // WP-R1 (TR-2): no sender writes a topic, so a subscription topic filtered out every message.
    const FString IgnoredTopic = O3DNNG::FindIgnoredTopic(Config);
    if (!IgnoredTopic.IsEmpty())
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG subscription topics are not supported; ignoring '%s' and receiving every message."), *IgnoredTopic);
    }

    CapabilityMode.store(Options.Mode);
    ActiveConfig = Config;
    ActiveConfig.Uri = Options.CanonicalUri;
    ActiveConfig.StreamId = Options.StreamId;
    ActiveAudioConfig = Config.Audio;
    // Note: Audio stream label is now automatically derived from StreamId

    // The consumer gets Options.StreamId as its stream, as before; messages above the receive
    // limit NNG enforces are refused by the demux too.
    FO3DReceiveDemuxSettings DemuxSettings = Demux.GetSettings();
    DemuxSettings.StreamId = Options.StreamId;
    DemuxSettings.MaxMessageBytes = static_cast<int32>(O3DNngReceiverPrivate::MaxPayloadBytes);
    Demux.SetSettings(DemuxSettings);
    Demux.ResetStats();

    FramesReceived.store(0);
    BytesReceived.store(0);
    ReceiveErrors.store(0);
    PipeContext->PipeCount.store(0);
    PipeContext->bConnectedWithoutPipes.store(Options.bListen);
    LastErrorLogTimestamp = 0.0;

    bInitialized = true;
    return FO3DTransportResult::Ok();
}

void FO3DNngReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
    Demux.SetAudioSink(Sink);
    if (Sink.IsValid())
    {
        ActiveAudioConfig = AudioConfig;
        // Note: Audio stream label is now automatically derived from StreamId
    }
}

FO3DNngReceiver::~FO3DNngReceiver()
{
    Stop();
    // After nng_close() a late pipe callback resolves the token to nothing (TRB-42).
    O3DNngReceiverPrivate::GetReceiverPipeContextRegistry().Unregister(PipeToken);
    PipeToken = nullptr;
}

FO3DTransportResult FO3DNngReceiver::Start()
{
    if (!bInitialized.load())
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver Start called before Initialize"));
        return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("NNG receiver Start() before a successful Initialize()."));
    }

    if (bRunning.load())
    {
        return FO3DTransportResult::Ok();
    }

    if (!Demux.HasConsumer())
    {
        return FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("NNG receiver Start() without a frame consumer (SetConsumer)."));
    }

    ReopenPolicy.Reset();

    int32 OpenError = 0;
    const bool bOpened = OpenSocket(&OpenError);
    if (!bOpened && Options.bListen)
    {
        // OpenSocket logged the reason (for example, the port is in use).
        const FO3DTransportResult Result = FO3DTransportResult::Error(
            OpenError == NNG_EADDRINUSE ? EO3DTransportError::AddressInUse : EO3DTransportError::ConnectFailed,
            FString::Printf(TEXT("NNG receiver could not listen on %s (%d %s)."), *O3DRedact::Url(Options.CanonicalUri), OpenError,
                OpenError != 0 ? UTF8_TO_TCHAR(nng_strerror(OpenError)) : TEXT("")));
        ConnectionState.End(EO3DConnectionState::Failed, Result);
        return Result;
    }
    if (!bOpened)
    {
        ReopenPolicy.OnFailure(FPlatformTime::Seconds());
    }
    if (Options.bListen && !O3DTransportOptions::IsLoopbackHost(Options.Host))
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver listens on %s:%d, reachable from other machines. The stream has no authentication or encryption; set 127.0.0.1 to accept only this machine (USER_GUIDE, Network Exposure)."), *Options.Host, Options.Port);
    }
    bRunning = true;
    bSawPeer = false;
    // No peer pipe yet; Poll reports the first one (a dialer keeps retrying in the background).
    ConnectionState.Begin(EO3DConnectionState::Connecting);

    UE_LOG(LogO3DNngReceiver, Log, TEXT("NNG receiver started - Mode=%s Role=%s URI=%s"),
        *O3DNNG::ModeToString(Options.Mode),
        *O3DNNG::RoleToString(Options.Role),
        *O3DRedact::Url(Options.CanonicalUri));
    return FO3DTransportResult::Ok();
}

void FO3DNngReceiver::Stop()
{
    CloseSocket();
    // The consumer and both sinks are released here, never called again after Stop (TRF-38).
    Demux.ReleaseSinks();
    if (bRunning.load())
    {
        bRunning = false;
        PipeContext->bConnected.store(false);
    }
    ConnectionState.End(EO3DConnectionState::Idle);
}

/**
 * Takes up to FramesPerPoll messages NNG already received (nng_recv, NNG_FLAG_NONBLOCK |
 * NNG_FLAG_ALLOC) and routes each through the demux. Counts every message once (TRB-42): mocap in
 * FramesReceived and BytesReceived, rejects (bad audio, malformed, too large) and receive errors
 * in ReceiveErrors. Reopens a socket that failed to listen or dial when the policy allows.
 */
int32 FO3DNngReceiver::Poll()
{
    if (!bRunning.load())
    {
        return 0;
    }

    // Pipes are added and removed asynchronously by NNG, so checking before a reopen is enough.
    UpdateConnectionState();

    if (!EnsureSocket())
    {
        return 0;
    }

    int32 FramesProcessed = 0;
    // WP-R1 (TR-3): every message counts towards the bound, not only frames, so control,
    // keepalive, empty or malformed messages cannot keep the game thread in one Poll.
    int32 MessagesTaken = 0;

    while (MessagesTaken < FO3DNngReceiver::FramesPerPoll && Socket)
    {
        ++MessagesTaken;
        void* Buffer = nullptr;
        size_t Size = 0;
        const int Ret = nng_recv(Socket->Socket, &Buffer, &Size, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC);
        if (Ret == NNG_EAGAIN || Ret == NNG_ETIMEDOUT)
        {
            break;
        }

        if (Ret != 0)
        {
            HandleReceiveError(Ret);
            break;
        }

        if (Size == 0)
        {
            nng_free(Buffer, Size);
            continue;
        }

        if (Size > O3DNngReceiverPrivate::MaxPayloadBytes)
        {
            int64 Suppressed = 0;
            if (NngOversizeLog.ShouldLog(Suppressed))
            {
                UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver payload %llu bytes exceeds safety cap; dropping. %lld oversized since the last warning."), static_cast<unsigned long long>(Size), Suppressed);
            }
            nng_free(Buffer, Size);
            ReceiveErrors.fetch_add(1);
            continue;
        }

        EO3DDemuxResult Result = EO3DDemuxResult::Malformed;
        ProcessReceivedPayload(static_cast<const uint8*>(Buffer), static_cast<int32>(Size), &Result);
        nng_free(Buffer, Size);

        switch (Result)
        {
        case EO3DDemuxResult::Mocap:
            FramesReceived.fetch_add(1);
            BytesReceived.fetch_add(static_cast<int64>(Size));
            ++FramesProcessed;
            break;
        case EO3DDemuxResult::Audio:
            ++FramesProcessed;
            break;
        case EO3DDemuxResult::AudioRejected:
        case EO3DDemuxResult::Malformed:
        case EO3DDemuxResult::Oversize:
            ReceiveErrors.fetch_add(1);
            break;
        default:
            break; // control (to the control sink, not a frame), keepalives, unknown kinds
        }
    }

    return FramesProcessed;
}

FO3DTransportStats FO3DNngReceiver::GetStats() const
{
    FO3DTransportStats Copy;
    Copy.FramesReceived = FramesReceived.load();
    Copy.BytesReceived = BytesReceived.load();
    Copy.ReceiveErrors = ReceiveErrors.load();
    Copy.State = ConnectionState.Get();
    return Copy;
}

void FO3DNngReceiver::UpdateConnectionState()
{
    // Pipe events arrive on NNG threads that hold only the pipe context, so Poll turns the pipe
    // count into connection-state changes (ADR 0007 item 3), on the game thread.
    const bool bHasPeer = Socket != nullptr && PipeContext->PipeCount.load() > 0;
    if (bHasPeer == bSawPeer)
    {
        return;
    }
    bSawPeer = bHasPeer;
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

/**
 * Opens the socket for Options.Mode (sub subscribes to everything: senders write no topic, WP-R1),
 * registers the pipe notifications and NNG_OPT_RECVMAXSZ before listen or dial, then
 * listens, or dials with NNG_FLAG_NONBLOCK. A listening socket counts as ready once it listens; a
 * dialing one only after its first pipe event (TRB-42).
 */
bool FO3DNngReceiver::OpenSocket(int32* OutNngError)
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
    case O3DNNG::ENngMode::Sub:
        Ret = nng_sub0_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            Ret = nng_setopt(NewSocket->Socket, NNG_OPT_SUB_SUBSCRIBE, "", 0);
        }
        if (Ret == 0)
        {
            // Receive buffer is an int counting messages (TRB-36). A sub socket drops messages
            // that arrive while this queue is full, and Poll() drains it only once per tick.
            constexpr int NngRecvBufMessages = 1024;
            const int SetRecvBufRet = nng_socket_set_int(NewSocket->Socket, NNG_OPT_RECVBUF, NngRecvBufMessages);
            if (SetRecvBufRet != 0)
            {
                UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver could not set receive buffer to %d messages (%d %s)"),
                    NngRecvBufMessages, SetRecvBufRet, UTF8_TO_TCHAR(nng_strerror(SetRecvBufRet)));
            }
        }
        break;
    case O3DNNG::ENngMode::Pair:
        Ret = nng_pair1_open(&NewSocket->Socket);
        break;
    case O3DNNG::ENngMode::Pull:
        Ret = nng_pull0_open(&NewSocket->Socket);
        break;
    default:
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver unsupported mode %s"), *O3DNNG::ModeToString(Options.Mode));
        delete NewSocket;
        return false;
    }

    if (Ret == 0)
    {
        // TRB-42: without this, NNG applies its own default receive limit (1 MiB per the NNG
        // option docs) and drops a larger message before Poll() sees it, so the MaxPayloadBytes
        // check in Poll was unreachable. recv-size-max is a size_t byte count.
        const int SetMaxSizeRet = nng_socket_set_size(NewSocket->Socket, NNG_OPT_RECVMAXSZ, static_cast<size_t>(O3DNngReceiverPrivate::MaxPayloadBytes));
        if (SetMaxSizeRet != 0)
        {
            UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver could not set the maximum message size to %llu bytes (%d %s)"),
                static_cast<unsigned long long>(O3DNngReceiverPrivate::MaxPayloadBytes), SetMaxSizeRet, UTF8_TO_TCHAR(nng_strerror(SetMaxSizeRet)));
        }

        // Notifications are registered before listen/dial so the first pipe event is not missed.
        PipeContext->PipeCount.store(0);
        PipeContext->bConnected.store(false);
        const int AddNotify = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_ADD_POST, O3DNngReceiverPrivate::ReceiverPipeCallback, PipeToken);
        if (AddNotify != 0)
        {
            UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver pipe notify add failed (%d) %s"), AddNotify, UTF8_TO_TCHAR(nng_strerror(AddNotify)));
        }
        const int RemoveNotify = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_REM_POST, O3DNngReceiverPrivate::ReceiverPipeCallback, PipeToken);
        if (RemoveNotify != 0)
        {
            UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver pipe notify remove failed (%d) %s"), RemoveNotify, UTF8_TO_TCHAR(nng_strerror(RemoveNotify)));
        }

        const FTCHARToUTF8 AddressUtf8(*Options.TcpAddress);
        if (Options.bListen)
        {
            Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
        }
        else
        {
            // Non-blocking dial: NNG retries in the background. "Connected" comes from pipe events.
            Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
        }
    }

    if (Ret != 0)
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver could not %s %s (%d) %s"),
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
        PipeContext->bConnected.store(true);
    }

    Socket = NewSocket;
    return true;
}

void FO3DNngReceiver::CloseSocket()
{
    if (Socket)
    {
        delete Socket;
        Socket = nullptr;
    }
}

void FO3DNngReceiver::HandleReceiveError(int ErrorCode)
{
    const double Now = FPlatformTime::Seconds();
    if (Now - LastErrorLogTimestamp > 2.0)
    {
        UE_LOG(LogO3DNngReceiver, Warning, TEXT("NNG receiver recv failed (%d) %s"), ErrorCode, UTF8_TO_TCHAR(nng_strerror(ErrorCode)));
        LastErrorLogTimestamp = Now;
    }

    ReceiveErrors.fetch_add(1);

    if (!Options.bListen)
    {
        // A dialer's socket is reopened by EnsureSocket when the policy allows.
        CloseSocket();
        ReopenPolicy.OnFailure(Now);
        PipeContext->bConnected.store(false);
    }
}

bool FO3DNngReceiver::EnsureSocket()
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

bool FO3DNngReceiver::ProcessReceivedPayload(const uint8* Data, int32 Size, EO3DDemuxResult* OutResult)
{
    // One classification for every message (ADR 0007 item 7): mocap to the consumer (without the
    // unified header, TRB-37; raw legacy frames unchanged), audio to the audio sink, control to the
    // control sink (ADR 0011); a damaged envelope is dropped, never passed on as mocap.
    const EO3DDemuxResult Result = Demux.ProcessMessage(Data, Size, FPlatformTime::Seconds());
    if (OutResult)
    {
        *OutResult = Result;
    }
    return Result == EO3DDemuxResult::Mocap || Result == EO3DDemuxResult::Audio;
}

#endif // O3D_WITH_TRANSPORT_NNG
