#pragma once

#include "CoreMinimal.h"

#include "Containers/Queue.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeRWLock.h"
#include "Templates/Atomic.h"
#include "Templates/Function.h"

/**
 * Marshals work from moq-ffi threads onto the game thread (WP-S8, TRF-13).
 *
 * Tasks go into a lock-free MPSC queue that a core ticker drains on the game thread. There is
 * no dispatcher thread. The module calls Initialize() at startup and Shutdown() before it
 * unloads moq-ffi. After Shutdown(), EnqueueGameThreadTask() drops new work and nothing
 * restarts the dispatcher lazily, so no module code runs on the game thread after
 * ShutdownModule returns.
 */
class FMoQAsyncDispatcher
{
public:
    static FMoQAsyncDispatcher& Get();

    /** Game thread. Starts accepting tasks and registers the draining ticker. Idempotent. */
    void Initialize();

    /** Game thread. Stops accepting tasks, removes the ticker and discards queued tasks. */
    void Shutdown();

    /** Any thread. Queues work for the game thread; returns false (and drops it) when shut down. */
    bool EnqueueGameThreadTask(TUniqueFunction<void()>&& Task);

    /**
     * Game thread. Runs every queued task now and returns how many ran. The ticker calls this
     * each frame; tests call it to deliver FFI callbacks deterministically.
     */
    int32 DrainOnGameThread();

    bool IsAcceptingTasks() const { return bAccepting.Load(); }

private:
    FMoQAsyncDispatcher() = default;

    TQueue<TUniqueFunction<void()>, EQueueMode::Mpsc> TaskQueue;
    /** Readers enqueue; Shutdown takes the write lock so no enqueue straddles it. */
    FRWLock AcceptLock;
    TAtomic<bool> bAccepting{false};
    FTSTicker::FDelegateHandle TickerHandle;
};
