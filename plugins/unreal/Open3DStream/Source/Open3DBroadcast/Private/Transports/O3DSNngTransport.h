#pragma once

#include "CoreMinimal.h"
#include "IBroadcastTransport.h"
#include "HAL/ThreadSafeCounter.h"
#include "Containers/Queue.h"          // TQueue, EQueueMode
#include "HAL/Event.h"                 // FEvent
#include "HAL/Runnable.h"              // FRunnable
#include "HAL/RunnableThread.h"        // FRunnableThread
#include "HAL/PlatformProcess.h"       // FPlatformProcess

// NNG-based transport supporting publisher (default) and pair modes
// URL forms:
//   - Publisher (default): tcp://host:port or tcp://host:port?mode=pub
//   - Pair client:         tcp://host:port?mode=pair&role=client
//   - Pair server:         tcp://host:port?mode=pair&role=server
// Notes: Query string is stripped before passing to nng APIs.

class FO3DSNngTransport : public IBroadcastTransport
{
public:
    FO3DSNngTransport();
    virtual ~FO3DSNngTransport() override;

    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) override;
    virtual void Stop() override;
    virtual bool Send(const uint8* Data, int32 Size, double TimestampSeconds) override;
    virtual bool IsConnected() const override { return bConnected.Load(); }
    virtual const FCounters& GetCounters() const override { return Counters; }

    virtual void Tick(float DeltaTime) override; // optional; worker handles most tasks

    // Worker entry (public so runnable can call it)
    uint32 WorkerRun();

    // Expose minimal state for static callbacks (kept public for C function interop)
    enum class EMode : uint8 { Pub, PairClient, PairServer };
    FThreadSafeCounter PipeCount;
    TAtomic<bool> bConnected{false};
    EMode Mode = EMode::Pub;

private:
    struct FItem { TArray<uint8> Data; double Ts = 0.0; };

    // URL parsing helpers
    bool ParseUrl(const FString& InUrl, FString& OutBaseUrl, TMap<FString, FString>& OutQuery) const;
    static FString StripQuery(const FString& InUrl);

    bool OpenSocket();
    void CloseSocket();

    // Worker thread logic
    void StartWorker();
    void StopWorker();

private:
    // Config
    FString Url;
    FString BaseUrl; // without query
    TMap<FString, FString> Query;

    // NNG state (opaque pointer to avoid including nng headers here)
    void* NngSocket = nullptr; // nng_socket* at runtime

    // Threading/queue
    TQueue<FItem, EQueueMode::Mpsc> Queue;
    FEvent* WakeEvent = nullptr;
    FRunnable* Worker = nullptr;
    FRunnableThread* Thread = nullptr;
    TAtomic<bool> bStop{false};

    // Transport-local backpressure
    TAtomic<uint64> QueueBytes{0};
    uint64 MaxQueueBytes = 4ull * 1024ull * 1024ull; // default 4 MB, override by CVar or URL ?qmax=<bytes>

    // Reconnect/backoff (pair client)
    double LastAttempt = 0.0;
    int32 BackoffAttempt = 0;

    // Counters
    FCounters Counters;

    // Internal guard for Start/Stop
    FCriticalSection StateMutex;
};
