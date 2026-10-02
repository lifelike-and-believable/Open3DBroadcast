// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Event.h"
#include "Math/RandomStream.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include <atomic>

class FO3DSendQueue;
class FRunnableThread;

/**
 * A transport's background thread (ADR 0007 item 7, WP-A1 step 4; ADR 0008 item 8).
 *
 * One FRunnable per transport instance that runs the transport's body in a loop: draining its
 * FO3DSendQueue onto the socket or FFI, and the connect, disconnect, reconnect and FFI lifecycle
 * calls that must stay off the game thread (TRF-7, WP-A5). SendSerialized and the audio sinks
 * only enqueue, so they never block (ADR 0007 item 3, ADR 0008); this thread is the only one that
 * touches the socket or FFI handle.
 *
 * The body returns how long to wait before it runs again. The wait ends early when the wake
 * queue (the transport's send queue, whose Enqueue wakes it) or Wake() is signalled, or when
 * Stop() is called. A body that loops for long should check IsStopRequested().
 *
 * Threading: Start, Stop: game thread (the transport's owner); Stop joins the thread, so it must
 * not be called from the body. Wake, IsRunning, IsStopRequested: any thread (Wake not concurrently
 * with Start, which sets the wake queue). The body may capture
 * the transport that owns the worker, because the transport's Stop() and destructor stop the
 * worker before anything the body uses goes away (the WP-S5 Stop ordering).
 */
class OPEN3DSHARED_API FO3DTransportWorker
{
public:
	/** One iteration of the transport's work. Returns the wait in ms before the next (0: run again at once). */
	using FBody = TFunction<uint32()>;

	FO3DTransportWorker();
	/** Stops and joins the thread if it still runs. */
	~FO3DTransportWorker();

	FO3DTransportWorker(const FO3DTransportWorker&) = delete;
	FO3DTransportWorker& operator=(const FO3DTransportWorker&) = delete;

	/**
	 * Starts the thread. WakeQueue is optional: when set, the worker waits on the queue's event,
	 * so every Enqueue wakes it. False when already running, Body is unset or the thread could not
	 * be created (then nothing runs).
	 */
	bool Start(const TCHAR* ThreadName, FBody InBody, TSharedPtr<FO3DSendQueue, ESPMode::ThreadSafe> InWakeQueue = nullptr);

	/** Asks the body to stop, wakes it and joins the thread. Idempotent. Never from the body itself. */
	void Stop();

	/** Wakes the worker early. Any thread. */
	void Wake();

	bool IsRunning() const { return bRunning.load(std::memory_order_acquire); }

	/** True between Stop() being called and the next Start(). For bodies that loop. */
	bool IsStopRequested() const { return bStopRequested.load(std::memory_order_acquire); }

	/** Number of times the body ran since Start. Any thread; for tests and diagnostics. */
	int64 GetIterations() const { return Iterations.load(std::memory_order_relaxed); }

private:
	/** The FRunnable that calls RunLoop; defined in the .cpp. */
	class FRunnableImpl;

	/** The loop the thread runs. */
	void RunLoop();
	void WaitFor(uint32 WaitMs);

	FBody Body;
	TSharedPtr<FO3DSendQueue, ESPMode::ThreadSafe> WakeQueue;
	FEventRef OwnEvent{ EEventMode::AutoReset };
	TUniquePtr<FRunnableImpl> Runnable;
	FRunnableThread* Thread = nullptr;
	std::atomic<bool> bStopRequested{ false };
	std::atomic<bool> bRunning{ false };
	std::atomic<int64> Iterations{ 0 };
};

/** Settings of FO3DReconnectPolicy. */
struct FO3DReconnectPolicySettings
{
	/** Delay before the first retry. */
	double InitialDelaySeconds = 0.5;
	/** Upper bound of a delay, jitter included. */
	double MaxDelaySeconds = 5.0;
	/** Growth per failed attempt (exponential backoff). Values below 1 are treated as 1. */
	double Multiplier = 2.0;
	/** Each delay is scaled by a random factor in [1 - JitterFraction, 1 + JitterFraction]; clamped to [0, 1]. */
	double JitterFraction = 0.2;
	/** Failed attempts after which the policy gives up (IsExhausted). 0 retries forever. */
	int32 MaxAttempts = 0;
};

/**
 * Reconnect backoff shared by every transport (ADR 0007 item 7; TRB-4, TRF-6, TRF-20, TRF-32):
 * exponential, with jitter so many receivers do not retry in lockstep, and reset on success.
 *
 * Usage on the worker: if (Policy.IsDue(Now)) { if (TryConnect()) Policy.OnSuccess(); else Policy.OnFailure(Now); }
 *
 * Not thread-safe: owned by one thread (the transport worker, or the game thread for transports
 * that reconnect from Tick). Deterministic for a given seed, so tests can check exact delays.
 */
class OPEN3DSHARED_API FO3DReconnectPolicy
{
public:
	/** Seed 0 picks a seed from the clock. */
	explicit FO3DReconnectPolicy(const FO3DReconnectPolicySettings& InSettings = FO3DReconnectPolicySettings(), int32 Seed = 0);

	/**
	 * Records a failed attempt at NowSec and schedules the next one. Returns the delay until it,
	 * or a negative value when MaxAttempts is reached (IsExhausted, IsDue stays false).
	 */
	double OnFailure(double NowSec);

	/** Records a successful attempt: the next failure starts again at InitialDelaySeconds. */
	void OnSuccess();

	/** Forgets every attempt; the next IsDue is true at once. */
	void Reset();

	/** True when an attempt may be made at NowSec (always true before the first failure). */
	bool IsDue(double NowSec) const;

	/** Failed attempts since the last success or reset. */
	int32 GetFailedAttempts() const { return FailedAttempts; }

	bool IsExhausted() const;

	/** The delay OnFailure would use for the Nth consecutive failure (1-based), before jitter. */
	double GetBaseDelaySeconds(int32 Failure) const;

	const FO3DReconnectPolicySettings& GetSettings() const { return Settings; }

private:
	FO3DReconnectPolicySettings Settings;
	FRandomStream Random;
	int32 FailedAttempts = 0;
	double NextAttemptSec = 0.0;
};
