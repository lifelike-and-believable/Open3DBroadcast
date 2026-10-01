// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>

/**
 * Connection state of one sender or receiver, with its state-changed callback (ADR 0007 item 3,
 * WP-A1 PR 3). Every in-tree transport keeps one and forwards GetConnectionState() and
 * SetStateChangedCallback() to it, so the threading rules below hold for all of them:
 *
 * - Get() is lock-free and may be called on any thread.
 * - SetCallback() is a game-thread call, made before Start (it may be made later; it then takes
 *   effect from the next change).
 * - A change runs the callback on the thread that made it, before Begin/Set/End returns. The
 *   state is updated and the callback run under one lock, so callbacks for one instance never
 *   overlap and arrive in the order the state changed. The callback must not call back into the
 *   transport or into this tracker.
 * - Begin() opens a session (Start), End() closes it (Stop, or a failed Start). Between End()
 *   and the next Begin(), Set() is ignored, so a worker or FFI thread that reports a change late
 *   cannot move a stopped transport out of Idle or call the callback after Stop returned.
 */
class OPEN3DSHARED_API FO3DConnectionStateTracker
{
public:
	FO3DConnectionStateTracker() = default;

	FO3DConnectionStateTracker(const FO3DConnectionStateTracker&) = delete;
	FO3DConnectionStateTracker& operator=(const FO3DConnectionStateTracker&) = delete;

	/** The current state. Lock-free; any thread. */
	EO3DConnectionState Get() const
	{
		return static_cast<EO3DConnectionState>(State.load(std::memory_order_acquire));
	}

	/** Replaces the callback; null removes it. Game thread. */
	void SetCallback(FO3DConnectionStateCallback InCallback);

	/** Opens a session and moves to InState (Connecting or Connected). Game thread (Start). */
	void Begin(EO3DConnectionState InState, const FO3DTransportResult& Reason = FO3DTransportResult());

	/**
	 * Moves to InState while a session is open. Returns true when the state changed (and the
	 * callback ran). Ignored, returning false, outside a session. Any thread.
	 */
	bool Set(EO3DConnectionState InState, const FO3DTransportResult& Reason = FO3DTransportResult());

	/**
	 * Closes the session and moves to InState: Idle for Stop, Failed for a Start that failed or a
	 * transport that gave up. Fires the callback when the state changed. Any thread.
	 */
	void End(EO3DConnectionState InState = EO3DConnectionState::Idle, const FO3DTransportResult& Reason = FO3DTransportResult());

	/** True between Begin and End. Any thread. */
	bool IsOpen() const
	{
		return bOpen.load(std::memory_order_acquire);
	}

private:
	/** Under Mutex: stores InState and runs the callback when it differs from the previous state. */
	bool ChangeLocked(EO3DConnectionState InState, const FO3DTransportResult& Reason);

	mutable FCriticalSection Mutex;
	FO3DConnectionStateCallback Callback;
	std::atomic<uint8> State{static_cast<uint8>(EO3DConnectionState::Idle)};
	std::atomic<bool> bOpen{false};
};
