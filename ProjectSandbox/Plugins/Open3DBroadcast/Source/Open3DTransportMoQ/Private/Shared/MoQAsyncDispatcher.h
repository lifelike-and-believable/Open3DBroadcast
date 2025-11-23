#pragma once

#include "CoreMinimal.h"

#include "Containers/Queue.h"
#include "HAL/Event.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/ThreadSafeBool.h"
#include "Misc/ScopeLock.h"
#include "Templates/Function.h"

/**
 * Background dispatcher used to marshal callbacks originating from the
 * moq-ffi runtime threads onto the Unreal game thread.
 */
class FMoQAsyncDispatcher : public FRunnable
{
public:
    static FMoQAsyncDispatcher& Get();

    /** Ensure the worker thread is running */
    void Initialize();

    /** Flush any pending work and stop the worker thread */
    void Shutdown();

    /** Queue a lambda that must eventually execute on the game thread */
    void EnqueueGameThreadTask(TUniqueFunction<void()>&& Task);

private:
    FMoQAsyncDispatcher() = default;

    virtual uint32 Run() override;
    virtual void Stop() override;

    void DrainQueue();

    TQueue<TUniqueFunction<void()>, EQueueMode::Mpsc> TaskQueue;
    FEvent* TaskEvent = nullptr;
    FRunnableThread* WorkerThread = nullptr;
    FThreadSafeBool bStopRequested = false;
    FCriticalSection InitMutex;
};
