#include "Transports/O3DSNngTransport.h"
#include "Open3DBroadcast.h"

// Use NNG via Open3DStream third-party includes
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pair1/pair.h>

extern "C" {
    // callback signature per nng.h
    typedef void (*nng_pipe_cb)(nng_pipe, nng_pipe_ev, void*);
}

namespace
{
    // Simple RAII wrapper for nng_socket hidden behind void*
    struct FNngSock
    {
        nng_socket Sock{ NNG_SOCKET_INITIALIZER };
        ~FNngSock() { if (Sock.id) { nng_close(Sock); } }
    };

    static void UrlSplitQuery(const FString& InUrl, FString& OutBase, TMap<FString,FString>& OutQuery)
    {
        int32 QIdx;
        if (InUrl.FindChar('?', QIdx))
        {
            OutBase = InUrl.Left(QIdx);
            const FString Qs = InUrl.Mid(QIdx + 1);
            TArray<FString> Pairs; Qs.ParseIntoArray(Pairs, TEXT("&"), true);
            for (const FString& P : Pairs)
            {
                FString K,V;
                if (P.Split(TEXT("="), &K, &V))
                {
                    OutQuery.Add(K.ToLower(), V.ToLower());
                }
                else
                {
                    OutQuery.Add(P.ToLower(), TEXT(""));
                }
            }
        }
        else
        {
            OutBase = InUrl;
        }
    }

    // Worker runnable that calls outer->WorkerRun()
    class FNNGWorker : public FRunnable
    {
    public:
        explicit FNNGWorker(FO3DSNngTransport* InOuter) : Outer(InOuter) {}
        virtual uint32 Run() override { return Outer ? Outer->WorkerRun() : 0; }
        virtual void Stop() override {}
    private:
        FO3DSNngTransport* Outer = nullptr;
    };

    // Static pipe callback that routes to instance
    static void StaticPipeCb(nng_pipe /*pipe*/, nng_pipe_ev ev, void* arg)
    {
        FO3DSNngTransport* Self = static_cast<FO3DSNngTransport*>(arg);
        if (!Self) return;
        if (ev == NNG_PIPE_EV_ADD_POST)
        {
            const int32 Count = Self->PipeCount.Increment();
            Self->bConnected = true;
            UE_LOG(LogO3DSBroadcast, Log, TEXT("NNG: Pipe added (count=%d)"), Count);
        }
        else if (ev == NNG_PIPE_EV_REM_POST)
        {
            const int32 Count = Self->PipeCount.Decrement();
            if (Count <= 0)
            {
                Self->bConnected = (Self->Mode != FO3DSNngTransport::EMode::PairClient);
            }
            UE_LOG(LogO3DSBroadcast, Log, TEXT("NNG: Pipe removed (count=%d)"), FMath::Max(0, Count));
        }
    }
}

FO3DSNngTransport::FO3DSNngTransport() {}
FO3DSNngTransport::~FO3DSNngTransport() { Stop(); }

bool FO3DSNngTransport::ParseUrl(const FString& InUrl, FString& OutBaseUrl, TMap<FString, FString>& OutQuery) const
{
    if (InUrl.IsEmpty()) { return false; }
    UrlSplitQuery(InUrl, OutBaseUrl, OutQuery);
    return !OutBaseUrl.IsEmpty();
}

FString FO3DSNngTransport::StripQuery(const FString& InUrl)
{
    int32 QIdx; return InUrl.FindChar('?', QIdx) ? InUrl.Left(QIdx) : InUrl;
}

bool FO3DSNngTransport::Start(const FString& InUrl, const FString& /*InProtocol*/, const FString& /*InKey*/)
{
    FScopeLock Lock(&StateMutex);

    Url = InUrl;
    Query.Reset();
    if (!ParseUrl(Url, BaseUrl, Query))
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("NNG: Invalid URL %s"), *Url);
        return false;
    }

    const FString* ModeStr = Query.Find(TEXT("mode"));
    const FString* RoleStr = Query.Find(TEXT("role"));
    if (ModeStr && *ModeStr == TEXT("pair"))
    {
        if (RoleStr && *RoleStr == TEXT("client")) Mode = EMode::PairClient;
        else                                        Mode = EMode::PairServer; // default server when pair without role
    }
    else
    {
        Mode = EMode::Pub; // default
    }

    // Allow queue cap override via query: qmax=bytes
    if (const FString* QMax = Query.Find(TEXT("qmax")))
    {
        uint64 Tmp = 0;
        if (LexTryParseString(Tmp, **QMax))
        {
            MaxQueueBytes = FMath::Clamp<uint64>(Tmp, 64 * 1024, 512ull * 1024ull * 1024ull);
        }
    }

    if (!OpenSocket())
    {
        return false;
    }

    WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
    bStop = false;
    StartWorker();

    UE_LOG(LogO3DSBroadcast, Log, TEXT("NNG: Started mode=%s url=%s (qmax=%llu)"),
        (Mode==EMode::Pub?TEXT("pub"):Mode==EMode::PairClient?TEXT("pair-client"):TEXT("pair-server")), *BaseUrl,
        (unsigned long long)MaxQueueBytes);

    return true;
}

void FO3DSNngTransport::Stop()
{
    FScopeLock Lock(&StateMutex);

    StopWorker();

    if (WakeEvent)
    {
        FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
        WakeEvent = nullptr;
    }

    CloseSocket();
    bConnected = false;
    QueueBytes = 0;
}

bool FO3DSNngTransport::OpenSocket()
{
    // Create specific socket
    FNngSock* Sock = new FNngSock();
    int ret = 0;

    if (Mode == EMode::Pub)
    {
        ret = nng_pub0_open(&Sock->Sock);
        if (ret != 0) { delete Sock; UE_LOG(LogO3DSBroadcast, Warning, TEXT("NNG: pub open failed %d (%s)"), ret, UTF8_TO_TCHAR(nng_strerror(ret))); return false; }
        ret = nng_listen(Sock->Sock, TCHAR_TO_ANSI(*BaseUrl), nullptr, 0);
        if (ret != 0) { delete Sock; UE_LOG(LogO3DSBroadcast, Warning, TEXT("NNG: listen failed %d (%s)"), ret, UTF8_TO_TCHAR(nng_strerror(ret))); return false; }
        bConnected = true; // listening
    }
    else if (Mode == EMode::PairServer)
    {
        ret = nng_pair1_open(&Sock->Sock);
        if (ret != 0) { delete Sock; UE_LOG(LogO3DSBroadcast, Warning, TEXT("NNG: pair open failed %d (%s)"), ret, UTF8_TO_TCHAR(nng_strerror(ret))); return false; }
        ret = nng_listen(Sock->Sock, TCHAR_TO_ANSI(*BaseUrl), nullptr, 0);
        if (ret != 0) { delete Sock; UE_LOG(LogO3DSBroadcast, Warning, TEXT("NNG: listen failed %d (%s)"), ret, UTF8_TO_TCHAR(nng_strerror(ret))); return false; }
        bConnected = true; // server ready; peer may not be connected yet
    }
    else // PairClient
    {
        ret = nng_pair1_open(&Sock->Sock);
        if (ret != 0) { delete Sock; UE_LOG(LogO3DSBroadcast, Warning, TEXT("NNG: pair open failed %d (%s)"), ret, UTF8_TO_TCHAR(nng_strerror(ret))); return false; }
        ret = nng_dial(Sock->Sock, TCHAR_TO_ANSI(*BaseUrl), nullptr, NNG_FLAG_NONBLOCK);
        if (ret != 0) { UE_LOG(LogO3DSBroadcast, Verbose, TEXT("NNG: initial dial failed %d (%s); will retry"), ret, UTF8_TO_TCHAR(nng_strerror(ret))); }
        // Treat as not yet connected; will become connected when a pipe is added
        bConnected = (ret == 0);
    }

    // Register pipe notifications for visibility
    int r1 = nng_pipe_notify(Sock->Sock, NNG_PIPE_EV_ADD_POST, StaticPipeCb, this);
    if (r1 != 0)
    {
        UE_LOG(LogO3DSBroadcast, Verbose, TEXT("NNG: pipe notify ADD failed %d (%s)"), r1, UTF8_TO_TCHAR(nng_strerror(r1)));
    }
    int r2 = nng_pipe_notify(Sock->Sock, NNG_PIPE_EV_REM_POST, StaticPipeCb, this);
    if (r2 != 0)
    {
        UE_LOG(LogO3DSBroadcast, Verbose, TEXT("NNG: pipe notify REM failed %d (%s)"), r2, UTF8_TO_TCHAR(nng_strerror(r2)));
    }

    NngSocket = Sock;
    return true;
}

void FO3DSNngTransport::CloseSocket()
{
    FNngSock* Sock = reinterpret_cast<FNngSock*>(NngSocket);
    if (Sock)
    {
        delete Sock; // RAII closes
        NngSocket = nullptr;
    }
}

bool FO3DSNngTransport::Send(const uint8* Data, int32 Size, double Ts)
{
    if (Size <= 0 || Data == nullptr) { return false; }

    // Transport-local backpressure
    const uint64 NewQueued = QueueBytes.Load() + (uint64)Size;
    if (MaxQueueBytes > 0 && NewQueued > MaxQueueBytes)
    {
        Counters.FramesDropped++;
        return false;
    }

    // Enqueue for worker
    FItem Item; Item.Ts = Ts; Item.Data.SetNumUninitialized(Size);
    FMemory::Memcpy(Item.Data.GetData(), Data, Size);
    Queue.Enqueue(MoveTemp(Item));
    QueueBytes.Store(NewQueued);
    if (WakeEvent) { WakeEvent->Trigger(); }
    return true;
}

void FO3DSNngTransport::StartWorker()
{
    if (!Thread)
    {
        Worker = new FNNGWorker(this);
        Thread = FRunnableThread::Create(Worker, TEXT("O3DS_NNG_Worker"));
    }
}

void FO3DSNngTransport::StopWorker()
{
    bStop = true;
    if (WakeEvent) { WakeEvent->Trigger(); }
    if (Thread)
    {
        Thread->WaitForCompletion();
        delete Thread; Thread = nullptr;
    }
    if (Worker)
    {
        delete Worker; Worker = nullptr;
    }

    // drain
    FItem Tmp; while (Queue.Dequeue(Tmp)) {}
    QueueBytes = 0;
}

uint32 FO3DSNngTransport::WorkerRun()
{
    while (!bStop.Load())
    {
        // Wait until we have something to send
        FItem Item;
        if (!Queue.Dequeue(Item))
        {
            if (WakeEvent)
            {
                WakeEvent->Wait(50); // 50ms
            }
            continue;
        }
        QueueBytes.Store(QueueBytes.Load() - (uint64)Item.Data.Num());

        FNngSock* Sock = reinterpret_cast<FNngSock*>(NngSocket);
        if (!Sock)
        {
            Counters.FramesDropped++;
            continue;
        }

        // Send; use nng_send (blocking would block the worker only)
        int ret = nng_send(Sock->Sock, Item.Data.GetData(), Item.Data.Num(), 0);
        if (ret != 0)
        {
            Counters.FramesDropped++;
            bConnected = false;
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("NNG: send failed %d (%s)"), ret, UTF8_TO_TCHAR(nng_strerror(ret)));
            if (Mode == EMode::PairClient)
            {
                // Close and redial with backoff
                CloseSocket();
                const double Now = FPlatformTime::Seconds();
                const double Delay = FMath::Min(5.0, FMath::Pow(2.0, (double)FMath::Clamp(BackoffAttempt, 0, 5)) * 0.1);
                if (Now - LastAttempt > Delay)
                {
                    LastAttempt = Now; ++BackoffAttempt; Counters.Reconnects++;
                    OpenSocket();
                }
            }
            else if (Mode == EMode::PairServer)
            {
                // On error, keep listening; peer likely disconnected
                bConnected = true; // server remains up
            }
            // Publisher stays up regardless; error just indicates no subscribers or transient pipe issue
            continue;
        }

        Counters.BytesSent += (uint64)Item.Data.Num();
        Counters.FramesSent++;
    }
    return 0;
}

void FO3DSNngTransport::Tick(float /*DeltaTime*/)
{
    // No-op; worker handles backoff for pair client
}
