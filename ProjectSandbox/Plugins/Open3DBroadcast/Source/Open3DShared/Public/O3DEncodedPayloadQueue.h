// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Event.h"

#include <atomic>

/**
 * Bounded multi-producer, single-consumer hand-off of already-encoded payloads
 * (ADR 0007 addendum, WP-S5; folds into FO3DSendQueue in WP-A1).
 *
 * Producers (audio threads, the game thread) call Enqueue(). One transport worker calls
 * Dequeue(). Byte accounting uses fetch_add/fetch_sub, so concurrent producers cannot
 * lose or double-count bytes. The queue owns its wake event, so a producer can trigger
 * it for as long as it holds a reference to the queue, independently of the sender.
 */
class OPEN3DSHARED_API FO3DEncodedPayloadQueue
{
public:
	explicit FO3DEncodedPayloadQueue(uint64 InMaxBytes = 0);
	FO3DEncodedPayloadQueue(const FO3DEncodedPayloadQueue&) = delete;
	FO3DEncodedPayloadQueue& operator=(const FO3DEncodedPayloadQueue&) = delete;

	/**
	 * Any thread. Returns false (and leaves Bytes untouched) if the payload is empty or
	 * would exceed the byte cap. bIgnoreCap is for a consumer putting an item back.
	 * Triggers the wake event on success.
	 */
	bool Enqueue(TArray<uint8>&& Bytes, bool bIgnoreCap = false);

	/** Consumer thread only. */
	bool Dequeue(TArray<uint8>& OutBytes);

	/** Discards everything. Only call when no other consumer is running. */
	void Empty();

	/** Game thread; takes effect for later Enqueue calls. 0 disables the cap. */
	void SetMaxBytes(uint64 InMaxBytes) { MaxBytes.store(InMaxBytes, std::memory_order_relaxed); }

	uint64 GetPendingBytes() const { return PendingBytes.load(std::memory_order_relaxed); }

	/** Wakes the consumer. Safe from any thread while the caller holds a reference. */
	void Wake() { WakeEvent->Trigger(); }

	/** Consumer thread: waits up to WaitMs for a Wake(). */
	void WaitForWork(uint32 WaitMs) { WakeEvent->Wait(WaitMs); }

private:
	TQueue<TArray<uint8>, EQueueMode::Mpsc> Queue;
	std::atomic<uint64> PendingBytes{0};
	std::atomic<uint64> MaxBytes{0};
	FEventRef WakeEvent{EEventMode::AutoReset};
};
