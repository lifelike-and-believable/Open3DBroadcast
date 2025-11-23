#include "Shared/MoQAsyncDispatcher.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Shared/MoQTypes.h"

namespace
{
    constexpr TCHAR DispatcherThreadName[] = TEXT("MoQAsyncDispatcher");
}

FMoQAsyncDispatcher& FMoQAsyncDispatcher::Get()
{
    static FMoQAsyncDispatcher Instance;
    return Instance;
}

void FMoQAsyncDispatcher::Initialize()
{
    FScopeLock Lock(&InitMutex);
    if (WorkerThread != nullptr)
    {
        return;
    }

    if (TaskEvent == nullptr)
    {
        TaskEvent = FPlatformProcess::GetSynchEventFromPool(false);
    }

    bStopRequested = false;
    WorkerThread = FRunnableThread::Create(this, DispatcherThreadName, 0, TPri_AboveNormal);
    if (WorkerThread == nullptr)
    {
        UE_LOG(LogMoQBridge, Error, TEXT("Failed to create MoQ async dispatcher thread"));
        bStopRequested = true;
        FPlatformProcess::ReturnSynchEventToPool(TaskEvent);
        TaskEvent = nullptr;
    }
}

void FMoQAsyncDispatcher::Shutdown()
{
    FScopeLock Lock(&InitMutex);
    if (WorkerThread == nullptr)
    {
        if (TaskEvent != nullptr)
        {
            FPlatformProcess::ReturnSynchEventToPool(TaskEvent);
            TaskEvent = nullptr;
        }
        return;
    }

    bStopRequested = true;
    if (TaskEvent != nullptr)
    {
        TaskEvent->Trigger();
    }

    WorkerThread->WaitForCompletion();
    delete WorkerThread;
    WorkerThread = nullptr;

    DrainQueue();

    if (TaskEvent != nullptr)
    {
        FPlatformProcess::ReturnSynchEventToPool(TaskEvent);
        TaskEvent = nullptr;
    }
}

void FMoQAsyncDispatcher::EnqueueGameThreadTask(TUniqueFunction<void()>&& Task)
{
    TaskQueue.Enqueue(MoveTemp(Task));

    if (TaskEvent == nullptr)
    {
        Initialize();
    }

    if (TaskEvent != nullptr)
    {
        TaskEvent->Trigger();
    }
}

uint32 FMoQAsyncDispatcher::Run()
{
    while (!bStopRequested)
    {
        if (TaskEvent != nullptr)
        {
            TaskEvent->Wait();
        }

        DrainQueue();
    }

    // Final drain to flush any remaining work
    DrainQueue();
    return 0;
}

void FMoQAsyncDispatcher::Stop()
{
    bStopRequested = true;
    if (TaskEvent != nullptr)
    {
        TaskEvent->Trigger();
    }
}

void FMoQAsyncDispatcher::DrainQueue()
{
    TUniqueFunction<void()> Task;
    while (TaskQueue.Dequeue(Task))
    {
        AsyncTask(ENamedThreads::GameThread, MoveTemp(Task));
    }
}
