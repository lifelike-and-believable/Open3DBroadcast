// Copyright (c) Open3DStream Contributors

#include "Sender/NngSender.h"
#include "O3DRedact.h"

#include "Logging/LogMacros.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "O3DAudioFrameCodec.h"
#include "O3DSenderAudioSinkBase.h"
#include "O3DFfiContextRegistry.h"
#include "O3DUnifiedMessage.h"
#include "O3DPerformanceMetrics.h"

#include "o3ds/model.h"

#include <vector>

#if !defined(NNG_STATIC_LIB)
#define NNG_STATIC_LIB 1
#endif

#include <nng/nng.h>
#include <nng/protocol/pair1/pair.h>
#include <nng/protocol/pipeline0/push.h>
#include <nng/protocol/pubsub0/pub.h>

DEFINE_LOG_CATEGORY_STATIC(LogO3DNngSender, Log, All);

namespace
{
    constexpr uint64 kMinQueueBytes = 64ull * 1024ull;
    constexpr uint64 kMaxQueueBytes = 512ull * 1024ull * 1024ull;
}

namespace
{
    TO3DFfiContextRegistry<FNngSenderPipeContext>& GetPipeContextRegistry()
    {
        static TO3DFfiContextRegistry<FNngSenderPipeContext> Registry;
        return Registry;
    }

    /** NNG pipe callback. `Context` is an opaque token, never a sender pointer (WP-S5). */
    static void SenderPipeCallback(nng_pipe /*Pipe*/, nng_pipe_ev Event, void* Context)
    {
        const TSharedPtr<FNngSenderPipeContext, ESPMode::ThreadSafe> Pipe = GetPipeContextRegistry().Resolve(Context);
        if (!Pipe.IsValid())
        {
            return;
        }

        if (Event == NNG_PIPE_EV_ADD_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_add(1) + 1;
            Pipe->bConnected.store(true);
            UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender connection established (pipe count=%d)"), Count);
        }
        else if (Event == NNG_PIPE_EV_REM_POST)
        {
            const int32 Count = Pipe->PipeCount.fetch_sub(1) - 1;
            if (Count <= 0)
            {
                Pipe->bConnected.store(Pipe->bConnectedWithoutPipes.load());
            }
            UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender connection lost (pipe count=%d)"), FMath::Max(0, Count));
        }
    }
}

/**
 * NNG audio sink (WP-S5: TRB-35, TRB-10, TRB-11). Encodes with its own encoders and hands the
 * unified message to the worker through the shared queue. Never references the sender.
 */
class FNngSenderAudioSink final : public FO3DGatedSenderAudioSink
{
public:
    FNngSenderAudioSink(TSharedRef<FNngSenderPublishState, ESPMode::ThreadSafe> InState, const FO3DTransportAudioConfig& InAudioConfig, FO3DSinkAudioEncoder::FSettings InEncoderSettings)
        : FO3DGatedSenderAudioSink(InAudioConfig, InState->Gate, MoveTemp(InEncoderSettings))
        , State(MoveTemp(InState))
    {
    }

protected:
    virtual bool OnSubmitGated(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
    {
        TArray<uint8> Unified;
        if (!GetEncoder().EncodeUnified(StreamLabel, State->LastSubject.Get(), Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Unified))
        {
            return false;
        }

        if (!State->SendQueue.Enqueue(MoveTemp(Unified)))
        {
            State->AudioDropped.fetch_add(1);
            return false;
        }
        return true;
    }

private:
    TSharedRef<FNngSenderPublishState, ESPMode::ThreadSafe> State;
};

class FO3DNngSender::FNngSenderRunnable final : public FRunnable
{
public:
    explicit FNngSenderRunnable(FO3DNngSender& InOwner)
        : Owner(InOwner)
    {
    }

    virtual uint32 Run() override
    {
        return Owner.RunWorker();
    }

    virtual void Stop() override
    {
        // Owner drives stop via atomics; nothing required here.
    }

private:
    FO3DNngSender& Owner;
};

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
    : PublishState(MakeShared<FNngSenderPublishState, ESPMode::ThreadSafe>())
    , PipeContext(MakeShared<FNngSenderPipeContext, ESPMode::ThreadSafe>())
{
    PipeToken = GetPipeContextRegistry().Register(PipeContext);
}

bool FO3DNngSender::Initialize(const FO3DTransportConfig& Config)
{
    FString Error;
    if (!O3DNNG::ParseSenderOptions(Config, Options, Error))
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("Failed to parse NNG sender config: %s"), *Error);
        return false;
    }

    Options.MaxQueueBytes = FMath::Clamp<uint64>(Options.MaxQueueBytes, kMinQueueBytes, kMaxQueueBytes);
    UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender queue limit set to %llu bytes"), Options.MaxQueueBytes);

    ActiveConfig = Config;
    ActiveAudioConfig = Config.Audio;
    // Note: Audio stream label is now automatically derived from StreamId
    AudioSourceGuid = FGuid::NewGuid();

    Stats.Reset();
    PublishState->SendQueue.SetMaxBytes(Options.MaxQueueBytes);
    PublishState->AudioDropped.store(0);
    PipeContext->PipeCount.store(0);
    PipeContext->bConnectedWithoutPipes.store(!((Options.Mode == O3DNNG::ENngMode::Pair && !Options.bListen) || Options.Mode == O3DNNG::ENngMode::Push));
    BackoffAttempt = 0;
    LastBackoffAttemptTime = 0.0;
    LastErrorLogTimestamp = 0.0;
    LastBackpressureLogTimestamp = 0.0;
    PublishState->LastSubject.Reset();

    bInitialized = true;
    PublishState->Gate->Open();
    return true;
}

FO3DNngSender::~FO3DNngSender()
{
    Stop();
    // nng_close() in Stop() has returned, so no pipe callback is running for this socket
    // (needs-FFI-verification); a late one would resolve the token to nothing anyway.
    GetPipeContextRegistry().Unregister(PipeToken);
    PipeToken = nullptr;
}

bool FO3DNngSender::Start()
{
    if (!bInitialized.Load())
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender Start called before Initialize"));
        return false;
    }

    FScopeLock Lock(&StateMutex);

    if (bRunning.Load())
    {
        return true;
    }

    const bool bOpened = OpenSocket();
    if (!bOpened && Options.bListen)
    {
        return false;
    }

    PublishState->Gate->Open();

    bStopWorker = false;
    StartWorker();
    bRunning = true;

    UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender STARTED - Mode=%s Role=%s URI=%s (queue=%llu bytes)"),
        *O3DNNG::ModeToString(Options.Mode),
        *O3DNNG::RoleToString(Options.Role),
        *O3DRedact::Url(Options.CanonicalUri),
        Options.MaxQueueBytes);

    if (!bOpened && !Options.bListen)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender will attempt to connect to %s with exponential backoff (check host/port)"), *O3DRedact::Url(Options.CanonicalUri));
    }

    return bOpened || !Options.bListen;
}

void FO3DNngSender::Stop()
{
    FScopeLock Lock(&StateMutex);

    // WP-S5 ordering: close the audio gate (waits for in-flight submits), join the worker,
    // close the socket, drain. The wake event belongs to the shared queue (TRB-12).
    // The gate is closed even when not running so a sink never outlives Stop().
    PublishState->Gate->Close();

    if (!bRunning.Load())
    {
        return;
    }

    bStopWorker = true;
    PublishState->SendQueue.Wake();

    StopWorker();

    CloseSocket();
    DrainQueue();

    bRunning = false;
    PipeContext->bConnected.store(false);
    UE_LOG(LogO3DNngSender, Log, TEXT("NNG sender stopped"));
}

bool FO3DNngSender::Send(const O3DS::SubjectList& List)
{
    if (!bInitialized.Load() || !bRunning.Load())
    {
        FO3DPerformanceMetrics::Get().RecordFrameDropped();
        return false;
    }

    // Record frame capture attempt
    FO3DPerformanceMetrics::Get().RecordFrameCaptured();
    FO3DPerformanceMetrics::Get().SetActiveSubjectCount(static_cast<int32>(List.mItems.size()));

    std::vector<char> Buffer;
    const double TimestampSeconds = FPlatformTime::Seconds();
    int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
    if (BytesWritten <= 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender failed to serialize subject list"));
        FO3DPerformanceMetrics::Get().RecordSerializationError();
        return false;
    }

    // Record serialization metrics
    FO3DPerformanceMetrics::Get().RecordBytesSerialized(BytesWritten);

    FString ObservedSubject;
    if (!List.mItems.empty() && List.mItems[0])
    {
        ObservedSubject = UTF8_TO_TCHAR(List.mItems[0]->mName.c_str());
    }

    return SendBytes(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten, ObservedSubject);
}

bool FO3DNngSender::SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double /*CaptureTimestampSec*/)
{
    if (!bInitialized.Load() || !bRunning.Load())
    {
        FO3DPerformanceMetrics::Get().RecordFrameDropped();
        return false;
    }

    if (Len <= 0)
    {
        return false;
    }

    // The caller (FO3DSenderSerializer) already serialized these bytes, not
    // Send(SubjectList&) - this IS the only place that records capture/
    // serialization metrics for this frame (Send() is dormant in the
    // normal per-frame pipeline; see O3DSenderSerializer.cpp), so recording
    // them here is not a double-count against anything.
    FO3DPerformanceMetrics::Get().RecordFrameCaptured();
    FO3DPerformanceMetrics::Get().RecordBytesSerialized(Len);

    return SendBytes(Data, Len, SubjectName);
}

/** Enqueue already-serialized bytes for transmission and record transport-level stats/subject bookkeeping. */
bool FO3DNngSender::SendBytes(const uint8* Data, int32 Len, const FString& SubjectName)
{
    if (!SubjectName.IsEmpty())
    {
        PublishState->LastSubject.Set(SubjectName);
    }

    if (!EnqueuePayload(Data, Len))
    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.DroppedFrames++;
        FO3DPerformanceMetrics::Get().RecordTransportFrameDropped();
        return false;
    }

    // Record successful send metrics
    FO3DPerformanceMetrics::Get().RecordBytesSent(Len);
    FO3DPerformanceMetrics::Get().RecordTransportFrameSent(TEXT("NNG"), Len);

    return true;
}

void FO3DNngSender::Tick(float /*DeltaSeconds*/)
{
    if (!bRunning.Load())
    {
        return;
    }

    if ((Options.Mode == O3DNNG::ENngMode::Pair && !Options.bListen) || Options.Mode == O3DNNG::ENngMode::Push)
    {
        if (!Socket)
        {
            const double Now = FPlatformTime::Seconds();
            const double Delay = FMath::Min(5.0, FMath::Pow(2.0, static_cast<double>(FMath::Clamp(BackoffAttempt, 0, 6))) * 0.1);
            if (Now - LastBackoffAttemptTime >= Delay)
            {
                if (OpenSocket())
                {
                    BackoffAttempt = 0;
                    PipeContext->bConnected.store(true);
                }
                else
                {
                    LastBackoffAttemptTime = Now;
                }
            }
        }
    }
}

FO3DTransportStats FO3DNngSender::GetStats() const
{
    FScopeLock Lock(&StatsMutex);
    FO3DTransportStats Copy = Stats;
    Copy.DroppedFrames += PublishState->AudioDropped.load();
    return Copy;
}

bool FO3DNngSender::OpenSocket()
{
    CloseSocket();

    FNngSocketWrapper* NewSocket = new FNngSocketWrapper();
    int Ret = 0;

    const FTCHARToUTF8 AddressUtf8(*Options.TcpAddress);

    switch (Options.Mode)
    {
    case O3DNNG::ENngMode::Pub:
        Ret = nng_pub0_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
        }
        PipeContext->bConnected.store(Ret == 0);
        break;
    case O3DNNG::ENngMode::Pair:
        Ret = nng_pair1_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            if (Options.bListen)
            {
                Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
                PipeContext->bConnected.store(Ret == 0);
            }
            else
            {
                Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
                PipeContext->bConnected.store(Ret == 0);
            }
        }
        break;
    case O3DNNG::ENngMode::Push:
        Ret = nng_push0_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
            PipeContext->bConnected.store(Ret == 0);
        }
        break;
    default:
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender unsupported mode"));
        delete NewSocket;
        return false;
    }

    if (Ret != 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender socket open failed (%d) %s"), Ret, UTF8_TO_TCHAR(nng_strerror(Ret)));
        delete NewSocket;
        Socket = nullptr;
        if ((Options.Mode == O3DNNG::ENngMode::Pair && !Options.bListen) || Options.Mode == O3DNNG::ENngMode::Push)
        {
            LastBackoffAttemptTime = FPlatformTime::Seconds();
            BackoffAttempt++;
        }
        return false;
    }

    PipeContext->PipeCount.store(0);
    int NotifyAdd = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_ADD_POST, SenderPipeCallback, PipeToken);
    if (NotifyAdd != 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender pipe notify add failed (%d) %s"), NotifyAdd, UTF8_TO_TCHAR(nng_strerror(NotifyAdd)));
    }
    int NotifyRem = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_REM_POST, SenderPipeCallback, PipeToken);
    if (NotifyRem != 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender pipe notify remove failed (%d) %s"), NotifyRem, UTF8_TO_TCHAR(nng_strerror(NotifyRem)));
    }

    // NNG's send buffer is an int counting messages (0-8192), not bytes (TRB-36). With the
    // default depth, a pub socket drops whatever overflows a subscriber's per-pipe queue, so a
    // burst of frames loses messages even on 127.0.0.1. The application queue (MaxQueueBytes)
    // remains the byte limit and backpressure point.
    constexpr int NngSendBufMessages = 1024;
    const int SetSendBufRet = nng_socket_set_int(NewSocket->Socket, NNG_OPT_SENDBUF, NngSendBufMessages);
    if (SetSendBufRet != 0)
    {
        UE_LOG(LogO3DNngSender, Warning, TEXT("NNG sender could not set send buffer to %d messages (%d %s)"),
            NngSendBufMessages, SetSendBufRet, UTF8_TO_TCHAR(nng_strerror(SetSendBufRet)));
    }

    // Set send timeout to prevent worker thread from blocking indefinitely on slow/dead connections
    // 30 second timeout allows for slow cloud links while preventing permanent hangs
    int SetTimeoutRet = nng_setopt_ms(NewSocket->Socket, NNG_OPT_SENDTIMEO, 30000);
    if (SetTimeoutRet != 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender set send timeout (result: %d %s)"),
            SetTimeoutRet, UTF8_TO_TCHAR(nng_strerror(SetTimeoutRet)));
    }

    Socket = NewSocket;
    LastBackoffAttemptTime = FPlatformTime::Seconds();
    return true;
}

void FO3DNngSender::CloseSocket()
{
    if (Socket)
    {
        delete Socket;
        Socket = nullptr;
    }
}

void FO3DNngSender::StartWorker()
{
    if (!WorkerThread)
    {
        Worker = new FNngSenderRunnable(*this);
        WorkerThread = FRunnableThread::Create(Worker, TEXT("O3D_NNG_Sender_Worker"));
    }
}

void FO3DNngSender::StopWorker()
{
    if (WorkerThread)
    {
        WorkerThread->WaitForCompletion();
        delete WorkerThread;
        WorkerThread = nullptr;
    }
    if (Worker)
    {
        delete Worker;
        Worker = nullptr;
    }

    bStopWorker = false;
}

uint32 FO3DNngSender::RunWorker()
{
    TArray<uint8> Bytes;
    while (!bStopWorker.Load())
    {
        if (!PublishState->SendQueue.Dequeue(Bytes))
        {
            PublishState->SendQueue.WaitForWork(50);
            continue;
        }

        const uint64 PayloadSize = static_cast<uint64>(Bytes.Num());

        FNngSocketWrapper* ActiveSocket = Socket;
        if (!ActiveSocket)
        {
            FScopeLock StatsLock(&StatsMutex);
            Stats.DroppedFrames++;
            continue;
        }

        const int Ret = nng_send(ActiveSocket->Socket, Bytes.GetData(), Bytes.Num(), NNG_FLAG_NONBLOCK);
        if (Ret == NNG_EAGAIN)
        {
            // Socket buffer full due to slow receiver/network - re-queue to retry later
            // This prevents blocking the worker thread on slow cloud connections
            // (byte accounting is atomic inside the shared queue).
            PublishState->SendQueue.Enqueue(MoveTemp(Bytes), /*bIgnoreCap=*/true);
            // Brief yield to avoid busy-spinning when consistently backed up
            FPlatformProcess::Sleep(0.001f);
            continue;
        }
        if (Ret != 0)
        {
            FScopeLock StatsLock(&StatsMutex);
            Stats.DroppedFrames++;
            HandleSendError(Ret);
            continue;
        }

        {
            FScopeLock StatsLock(&StatsMutex);
            Stats.FramesSent++;
            Stats.BytesSent += PayloadSize;
        }
    }

    return 0;
}

bool FO3DNngSender::EnqueuePayload(const uint8* Data, int32 Size)
{
    if (Size <= 0 || Data == nullptr)
    {
        return false;
    }

    TArray<uint8> Bytes(Data, Size);
    if (!PublishState->SendQueue.Enqueue(MoveTemp(Bytes)))
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - LastBackpressureLogTimestamp > 0.5)
        {
            UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender queue full (pending=%llu / limit=%llu bytes). Dropping frame."),
                PublishState->SendQueue.GetPendingBytes(),
                Options.MaxQueueBytes);
            LastBackpressureLogTimestamp = Now;
        }
        return false;
    }

    return true;
}

void FO3DNngSender::DrainQueue()
{
    PublishState->SendQueue.Empty();
}

void FO3DNngSender::HandleSendError(int ErrorCode)
{
    const double Now = FPlatformTime::Seconds();
    if (Now - LastErrorLogTimestamp > 0.25)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender send failed (%d) %s"), ErrorCode, UTF8_TO_TCHAR(nng_strerror(ErrorCode)));
        LastErrorLogTimestamp = Now;
    }

    if (Options.Mode == O3DNNG::ENngMode::Pair && !Options.bListen)
    {
        CloseSocket();
        BackoffAttempt = FMath::Min(BackoffAttempt + 1, 10);
        LastBackoffAttemptTime = Now;
        PipeContext->bConnected.store(false);
    }
    else if (Options.Mode == O3DNNG::ENngMode::Push)
    {
        CloseSocket();
        BackoffAttempt = FMath::Min(BackoffAttempt + 1, 10);
        LastBackoffAttemptTime = Now;
        PipeContext->bConnected.store(false);
    }
    else if (Options.Mode == O3DNNG::ENngMode::Pair && Options.bListen)
    {
        PipeContext->bConnected.store(true); // server remains available
    }
    else if (Options.Mode == O3DNNG::ENngMode::Pub)
    {
        PipeContext->bConnected.store(true); // publisher remains ready even if no subscribers
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

    return MakeShared<FNngSenderAudioSink, ESPMode::ThreadSafe>(PublishState, EffectiveConfig, MoveTemp(EncoderSettings));
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
