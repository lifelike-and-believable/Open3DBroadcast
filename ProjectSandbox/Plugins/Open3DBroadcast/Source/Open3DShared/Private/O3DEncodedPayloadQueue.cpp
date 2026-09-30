// Copyright (c) Open3DStream Contributors

#include "O3DEncodedPayloadQueue.h"

FO3DEncodedPayloadQueue::FO3DEncodedPayloadQueue(uint64 InMaxBytes)
	: MaxBytes(InMaxBytes)
{
}

bool FO3DEncodedPayloadQueue::Enqueue(TArray<uint8>&& Bytes, bool bIgnoreCap)
{
	const uint64 Size = static_cast<uint64>(Bytes.Num());
	if (Size == 0)
	{
		return false;
	}

	const uint64 Previous = PendingBytes.fetch_add(Size, std::memory_order_relaxed);
	const uint64 Cap = MaxBytes.load(std::memory_order_relaxed);
	if (!bIgnoreCap && Cap > 0 && Previous + Size > Cap)
	{
		PendingBytes.fetch_sub(Size, std::memory_order_relaxed);
		return false;
	}

	Queue.Enqueue(MoveTemp(Bytes));
	WakeEvent->Trigger();
	return true;
}

bool FO3DEncodedPayloadQueue::Dequeue(TArray<uint8>& OutBytes)
{
	if (!Queue.Dequeue(OutBytes))
	{
		return false;
	}

	PendingBytes.fetch_sub(static_cast<uint64>(OutBytes.Num()), std::memory_order_relaxed);
	return true;
}

void FO3DEncodedPayloadQueue::Empty()
{
	TArray<uint8> Discard;
	while (Dequeue(Discard))
	{
	}
}
