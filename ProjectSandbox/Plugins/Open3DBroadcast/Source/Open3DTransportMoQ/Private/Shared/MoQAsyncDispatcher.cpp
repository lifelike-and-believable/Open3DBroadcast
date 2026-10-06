// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/MoQAsyncDispatcher.h"

#include "Shared/MoQTypes.h"

FMoQAsyncDispatcher& FMoQAsyncDispatcher::Get()
{
    static FMoQAsyncDispatcher Instance;
    return Instance;
}

void FMoQAsyncDispatcher::Initialize()
{
    check(IsInGameThread());

    {
        FWriteScopeLock Lock(AcceptLock);
        bAccepting.Store(true);
    }

    if (!TickerHandle.IsValid())
    {
        TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float /*DeltaTime*/)
        {
            FMoQAsyncDispatcher::Get().DrainOnGameThread();
            return true;
        }));
    }
}

void FMoQAsyncDispatcher::Shutdown()
{
    check(IsInGameThread());

    {
        // After this block no thread is inside EnqueueGameThreadTask and later calls drop work.
        FWriteScopeLock Lock(AcceptLock);
        bAccepting.Store(false);
    }

    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
        TickerHandle = FTSTicker::FDelegateHandle();
    }

    // Discard, not run: the objects these tasks would update are being torn down.
    TUniqueFunction<void()> Task;
    int32 Discarded = 0;
    while (TaskQueue.Dequeue(Task))
    {
        ++Discarded;
    }
    if (Discarded > 0)
    {
        UE_LOG(LogMoQBridge, Verbose, TEXT("MoQ dispatcher discarded %d task(s) at shutdown"), Discarded);
    }
}

bool FMoQAsyncDispatcher::EnqueueGameThreadTask(TUniqueFunction<void()>&& Task)
{
    FReadScopeLock Lock(AcceptLock);
    if (!bAccepting.Load())
    {
        return false;
    }
    TaskQueue.Enqueue(MoveTemp(Task));
    return true;
}

int32 FMoQAsyncDispatcher::DrainOnGameThread()
{
    check(IsInGameThread());

    int32 Ran = 0;
    TUniqueFunction<void()> Task;
    while (TaskQueue.Dequeue(Task))
    {
        if (Task)
        {
            Task();
        }
        ++Ran;
    }
    return Ran;
}

#endif // O3D_WITH_TRANSPORT_MOQ
