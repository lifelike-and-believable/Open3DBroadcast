// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

#include <atomic>

/**
 * Lifetime gate between a transport and the audio, capture or FFI threads that feed it
 * (ADR 0007 addendum, WP-S5).
 *
 * The gate is an FRWLock plus an atomic "open epoch". Zero means closed. Producers
 * (audio sinks) remember the epoch that was current when they were created and enter
 * with FReadScope. Stop() calls Close(), which returns only after every producer that
 * was inside has left, so handles can be released right after it.
 *
 * Threading:
 * - Open() and Close() are called from the game thread (the thread that owns the
 *   transport). Close() blocks only for the duration of producers already inside,
 *   which by contract do one encode and one queue hand-off.
 * - FReadScope may be used from any thread. It must not be nested on one thread
 *   and must not be held across a blocking socket call.
 */
class OPEN3DSHARED_API FO3DLifetimeGate
{
public:
	FO3DLifetimeGate() = default;
	FO3DLifetimeGate(const FO3DLifetimeGate&) = delete;
	FO3DLifetimeGate& operator=(const FO3DLifetimeGate&) = delete;

	/** Opens a new epoch if the gate is closed and returns the current epoch (never 0). */
	uint64 Open();

	/** Closes the gate and waits for in-flight producers to leave. Idempotent. */
	void Close();

	/** Current epoch, or 0 when closed. */
	uint64 GetEpoch() const { return OpenEpoch.load(std::memory_order_acquire); }

	bool IsOpen() const { return GetEpoch() != 0; }

	/** RAII read scope. Evaluates to true only if the gate is open in the expected epoch. */
	class OPEN3DSHARED_API FReadScope
	{
	public:
		FReadScope(FO3DLifetimeGate& InGate, uint64 ExpectedEpoch);
		~FReadScope();

		FReadScope(const FReadScope&) = delete;
		FReadScope& operator=(const FReadScope&) = delete;

		explicit operator bool() const { return bEntered; }

	private:
		FO3DLifetimeGate& Gate;
		bool bEntered = false;
	};

private:
	mutable FRWLock Lock;
	std::atomic<uint64> OpenEpoch{0};
	uint64 NextEpoch = 0; // Game thread only.
};
