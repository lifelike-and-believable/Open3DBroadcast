// Copyright (c) Open3DStream Contributors

#include "Sender/NngSender.h"

#include "Logging/LogMacros.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "O3DAudioFrameCodec.h"
#include "O3DSenderAudioSinkBase.h"
#include "O3DUnifiedMessage.h"

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

/**
 * Audio sink implementation for NNG sender that forwards captured audio through the configured codec.
 */
class FO3DNngSender::FNngSenderAudioSink final : public FO3DSenderAudioSinkBase
{
public:
    explicit FNngSenderAudioSink(FO3DNngSender& InOwner, const FO3DTransportAudioConfig& InAudioConfig)
        : FO3DSenderAudioSinkBase(InAudioConfig)
        , Owner(InOwner)
    {
    }

    virtual bool OnSubmitPcmInternal(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
    {
        return Owner.ProcessCapturedAudio(StreamLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
    }

private:
    FO3DNngSender& Owner;
};

namespace
{

    static void SenderPipeCallback(nng_pipe /*Pipe*/, nng_pipe_ev Event, void* Context)
    {
        FO3DNngSender* Sender = static_cast<FO3DNngSender*>(Context);
        if (!Sender)
        {
            return;
        }

        if (Event == NNG_PIPE_EV_ADD_POST)
        {
            Sender->HandlePipeAdded();
        }
        else if (Event == NNG_PIPE_EV_REM_POST)
        {
            Sender->HandlePipeRemoved();
        }
    }
}

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

void FO3DNngSender::HandlePipeAdded()
{
    const int32 Count = PipeCount.Increment();
    bConnected = true;
    BackoffAttempt = 0;
    UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender CONNECTION ESTABLISHED to %s (pipe count=%d)"), *Options.CanonicalUri, Count);
}

void FO3DNngSender::HandlePipeRemoved()
{
    const int32 Count = PipeCount.Decrement();
    if (Count <= 0)
    {
        if (Options.Mode == O3DNNG::ENngMode::Pair && !Options.bListen)
        {
            bConnected = false;
        }
        else if (Options.Mode == O3DNNG::ENngMode::Push)
        {
            bConnected = false;
        }
        else
        {
            bConnected = true; // listening sockets remain available
        }
    }
    UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender CONNECTION LOST from %s (pipe count=%d)"), *Options.CanonicalUri, FMath::Max(0, Count));
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
    if (ActiveAudioConfig.StreamLabel.IsEmpty())
    {
        ActiveAudioConfig.StreamLabel = Config.StreamId;
    }
    AudioSourceGuid = FGuid::NewGuid();
    RefreshAudioEncoder();

    Stats.Reset();
    QueueBytes = 0;
    PipeCount.Reset();
    BackoffAttempt = 0;
    LastBackoffAttemptTime = 0.0;
    LastErrorLogTimestamp = 0.0;
    LastBackpressureLogTimestamp = 0.0;

    bInitialized = true;
    return true;
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

    if (!WakeEvent)
    {
        WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
    }

    bStopWorker = false;
    StartWorker();
    bRunning = true;

    UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender STARTED - Mode=%s Role=%s URI=%s (queue=%llu bytes)"),
        *O3DNNG::ModeToString(Options.Mode),
        *O3DNNG::RoleToString(Options.Role),
        *Options.CanonicalUri,
        Options.MaxQueueBytes);

    if (!bOpened && !Options.bListen)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender will attempt to connect to %s with exponential backoff (check host/port)"), *Options.CanonicalUri);
    }

    return bOpened || !Options.bListen;
}

void FO3DNngSender::Stop()
{
    FScopeLock Lock(&StateMutex);

    if (!bRunning.Load())
    {
        return;
    }

    bStopWorker = true;
    if (WakeEvent)
    {
        WakeEvent->Trigger();
    }

    StopWorker();

    if (WakeEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
        WakeEvent = nullptr;
    }

    CloseSocket();
    DrainQueue();

    bRunning = false;
    bConnected = false;
    UE_LOG(LogO3DNngSender, Log, TEXT("NNG sender stopped"));
}

bool FO3DNngSender::Send(const O3DS::SubjectList& List)
{
    if (!bInitialized.Load() || !bRunning.Load())
    {
        return false;
    }

    std::vector<char> Buffer;
    const double TimestampSeconds = FPlatformTime::Seconds();
    int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
    if (BytesWritten <= 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender failed to serialize subject list"));
        return false;
    }

    if (!EnqueuePayload(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten))
    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.DroppedFrames++;
        return false;
    }

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
                    bConnected = true;
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
    return Stats;
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
        bConnected = (Ret == 0);
        break;
    case O3DNNG::ENngMode::Pair:
        Ret = nng_pair1_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            if (Options.bListen)
            {
                Ret = nng_listen(NewSocket->Socket, AddressUtf8.Get(), nullptr, 0);
                bConnected = (Ret == 0);
            }
            else
            {
                Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
                bConnected = (Ret == 0);
            }
        }
        break;
    case O3DNNG::ENngMode::Push:
        Ret = nng_push0_open(&NewSocket->Socket);
        if (Ret == 0)
        {
            Ret = nng_dial(NewSocket->Socket, AddressUtf8.Get(), nullptr, NNG_FLAG_NONBLOCK);
            bConnected = (Ret == 0);
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

    PipeCount.Reset();
    int NotifyAdd = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_ADD_POST, SenderPipeCallback, this);
    if (NotifyAdd != 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender pipe notify add failed (%d) %s"), NotifyAdd, UTF8_TO_TCHAR(nng_strerror(NotifyAdd)));
    }
    int NotifyRem = nng_pipe_notify(NewSocket->Socket, NNG_PIPE_EV_REM_POST, SenderPipeCallback, this);
    if (NotifyRem != 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender pipe notify remove failed (%d) %s"), NotifyRem, UTF8_TO_TCHAR(nng_strerror(NotifyRem)));
    }

    // Increase NNG's internal send buffer to handle cloud network latency
    // This prevents NNG from hitting backpressure before our application queue does
    int SetSendBufRet = nng_setopt_size(NewSocket->Socket, NNG_OPT_SENDBUF, Options.MaxQueueBytes);
    if (SetSendBufRet != 0)
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender set send buffer to %llu bytes (result: %d %s)"),
            Options.MaxQueueBytes, SetSendBufRet, UTF8_TO_TCHAR(nng_strerror(SetSendBufRet)));
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
    while (!bStopWorker.Load())
    {
        FQueuedPayload Payload;
        if (!Queue.Dequeue(Payload))
        {
            if (WakeEvent)
            {
                WakeEvent->Wait(50);
            }
            continue;
        }

        const uint64 PayloadSize = static_cast<uint64>(Payload.Bytes.Num());
        const uint64 Current = QueueBytes.Load();
        QueueBytes.Store(Current > PayloadSize ? Current - PayloadSize : 0);

        FNngSocketWrapper* ActiveSocket = Socket;
        if (!ActiveSocket)
        {
            FScopeLock StatsLock(&StatsMutex);
            Stats.DroppedFrames++;
            continue;
        }

        const int Ret = nng_send(ActiveSocket->Socket, Payload.Bytes.GetData(), Payload.Bytes.Num(), NNG_FLAG_NONBLOCK);
        if (Ret == NNG_EAGAIN)
        {
            // Socket buffer full due to slow receiver/network - re-queue to retry later
            // This prevents blocking the worker thread on slow cloud connections
            Queue.Enqueue(MoveTemp(Payload));
            // Re-add the payload size back to queue counter since send failed
            const uint64 PreviousCurrent = Current > PayloadSize ? Current - PayloadSize : 0;
            QueueBytes.Store(PreviousCurrent + PayloadSize);
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

    const uint64 Pending = QueueBytes.Load();
    if (Options.MaxQueueBytes > 0 && Pending + static_cast<uint64>(Size) > Options.MaxQueueBytes)
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - LastBackpressureLogTimestamp > 0.5)
        {
            UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender queue full (pending=%llu / limit=%llu bytes). Dropping frame."),
                Pending,
                Options.MaxQueueBytes);
            LastBackpressureLogTimestamp = Now;
        }
        return false;
    }

    FQueuedPayload Payload;
    Payload.Bytes.SetNumUninitialized(Size);
    FMemory::Memcpy(Payload.Bytes.GetData(), Data, Size);

    Queue.Enqueue(MoveTemp(Payload));
    QueueBytes.Store(Pending + static_cast<uint64>(Size));

    if (WakeEvent)
    {
        WakeEvent->Trigger();
    }

    return true;
}

void FO3DNngSender::DrainQueue()
{
    FQueuedPayload Payload;
    while (Queue.Dequeue(Payload))
    {
        // release payload
    }
    QueueBytes.Store(0);
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
        bConnected = false;
    }
    else if (Options.Mode == O3DNNG::ENngMode::Push)
    {
        CloseSocket();
        BackoffAttempt = FMath::Min(BackoffAttempt + 1, 10);
        LastBackoffAttemptTime = Now;
        bConnected = false;
    }
    else if (Options.Mode == O3DNNG::ENngMode::Pair && Options.bListen)
    {
        bConnected = true; // server remains available
    }
    else if (Options.Mode == O3DNNG::ENngMode::Pub)
    {
        bConnected = true; // publisher remains ready even if no subscribers
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
    if (EffectiveConfig.StreamLabel.IsEmpty())
    {
        EffectiveConfig.StreamLabel = ActiveConfig.StreamId;
    }

    ActiveAudioConfig = EffectiveConfig;
    RefreshAudioEncoder();

    return MakeShared<FNngSenderAudioSink, ESPMode::ThreadSafe>(*this, EffectiveConfig);
}

void FO3DNngSender::RefreshAudioEncoder()
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

    FString StreamLabelFallback = ActiveAudioConfig.StreamLabel;
    if (StreamLabelFallback.IsEmpty())
    {
        StreamLabelFallback = SubjectFallback;
    }

    bAudioEncoderInitialized = AudioEncoder.Initialize(ActiveAudioConfig, StreamLabelFallback, SubjectFallback);
}

bool FO3DNngSender::ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
    if (!bInitialized.Load() || !bRunning.Load() || !bAudioEncoderInitialized)
    {
        return false;
    }

    FString SubjectFallback = ActiveConfig.StreamId;
    if (SubjectFallback.IsEmpty())
    {
        SubjectFallback = Options.StreamId;
    }
    if (SubjectFallback.IsEmpty())
    {
        SubjectFallback = TEXT("nng");
    }

    O3DAudio::FEncodedFrame Frame;
    if (!AudioEncoder.BuildEncodedFrame(StreamLabel, SubjectFallback, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Frame))
    {
        return false;
    }

    if (Frame.Meta.SubjectName.IsEmpty())
    {
        Frame.Meta.SubjectName = SubjectFallback;
    }
    Frame.Meta.SourceGuid = AudioSourceGuid;

    return SendEncodedAudio(Frame, TimestampSec);
}

bool FO3DNngSender::SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec)
{
    if (!bInitialized.Load() || !bRunning.Load() || Frame.Encoded.Num() <= 0)
    {
        return false;
    }

    if (!O3DAudio::CreateUnifiedAudioMessage(Frame, TimestampSec, UnifiedAudioScratch))
    {
        UE_LOG(LogO3DNngSender, Verbose, TEXT("NNG sender failed to create unified audio message"));
        return false;
    }

    if (!EnqueuePayload(UnifiedAudioScratch.GetData(), UnifiedAudioScratch.Num()))
    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.DroppedFrames++;
        return false;
    }

    {
        FScopeLock StatsLock(&StatsMutex);
        Stats.FramesSent++;
        Stats.BytesSent += UnifiedAudioScratch.Num();
    }

    return true;
}

