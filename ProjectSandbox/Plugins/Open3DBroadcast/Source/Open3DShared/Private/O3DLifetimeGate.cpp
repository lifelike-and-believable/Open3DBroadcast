// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#include "O3DLifetimeGate.h"

uint64 FO3DLifetimeGate::Open()
{
	const uint64 Current = OpenEpoch.load(std::memory_order_acquire);
	if (Current != 0)
	{
		return Current;
	}

	// Epochs are never reused, so a producer created in an earlier session can never
	// re-enter after a Stop()/Start() cycle.
	++NextEpoch;
	if (NextEpoch == 0)
	{
		++NextEpoch;
	}

	// Take the write lock so the transition is ordered with any reader that is
	// re-checking the epoch under the read lock.
	Lock.WriteLock();
	OpenEpoch.store(NextEpoch, std::memory_order_release);
	Lock.WriteUnlock();
	return NextEpoch;
}

void FO3DLifetimeGate::Close()
{
	// Reject new producers first (fast path in FReadScope), then wait for the ones
	// already inside: WriteLock() cannot be acquired while any read lock is held.
	OpenEpoch.store(0, std::memory_order_release);
	Lock.WriteLock();
	Lock.WriteUnlock();
}

FO3DLifetimeGate::FReadScope::FReadScope(FO3DLifetimeGate& InGate, uint64 ExpectedEpoch)
	: Gate(InGate)
{
	if (ExpectedEpoch == 0 || Gate.OpenEpoch.load(std::memory_order_acquire) != ExpectedEpoch)
	{
		return;
	}

	Gate.Lock.ReadLock();
	if (Gate.OpenEpoch.load(std::memory_order_acquire) != ExpectedEpoch)
	{
		Gate.Lock.ReadUnlock();
		return;
	}
	bEntered = true;
}

FO3DLifetimeGate::FReadScope::~FReadScope()
{
	if (bEntered)
	{
		Gate.Lock.ReadUnlock();
	}
}
