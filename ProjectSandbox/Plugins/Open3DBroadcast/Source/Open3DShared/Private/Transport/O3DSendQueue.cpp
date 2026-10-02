// Copyright Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DSendQueue.h"

#include "HAL/PlatformTime.h"

namespace O3DSendQueuePrivate
{
	/** Hard cap from a soft cap: twice it with DropOldest, the cap itself otherwise. 0 stays "no limit". */
	template <typename T>
	T HardCap(T SoftCap, bool bDoubles)
	{
		if (SoftCap <= 0)
		{
			return 0;
		}
		if (!bDoubles)
		{
			return SoftCap;
		}
		const T Max = TNumericLimits<T>::Max();
		return SoftCap > Max / 2 ? Max : SoftCap * 2;
	}

	/**
	 * Adds Amount to Counter only if the result stays within Max (0 = no limit). A compare-exchange
	 * loop, so the counter never shows a value over its limit, not even transiently (TRB-3).
	 */
	template <typename T>
	bool TryReserve(std::atomic<T>& Counter, T Amount, T Max)
	{
		T Current = Counter.load(std::memory_order_acquire);
		do
		{
			if (Max > 0 && Current + Amount > Max)
			{
				return false;
			}
		}
		while (!Counter.compare_exchange_weak(Current, Current + Amount, std::memory_order_acq_rel, std::memory_order_acquire));
		return true;
	}
}

FO3DSendQueue::FO3DSendQueue(const FO3DSendQueueLimits& InLimits)
{
	SetLimits(InLimits);
}

FO3DSendQueue::~FO3DSendQueue() = default;

void FO3DSendQueue::SetLimits(const FO3DSendQueueLimits& InLimits)
{
	auto Store = [this](EO3DSendItemKind Kind, const FO3DSendQueueKindLimits& Limits)
	{
		FKindState& State = GetKind(Kind);
		State.MaxItems.store(FMath::Max(0, Limits.MaxItems), std::memory_order_relaxed);
		State.MaxBytes.store(FMath::Max<int64>(0, Limits.MaxBytes), std::memory_order_relaxed);
	};
	Store(EO3DSendItemKind::Mocap, InLimits.Mocap);
	Store(EO3DSendItemKind::Audio, InLimits.Audio);
	Store(EO3DSendItemKind::Control, InLimits.Control);
	MocapOverflow.store(static_cast<uint8>(InLimits.MocapOverflow), std::memory_order_relaxed);
	MaxAgeSeconds.store(FMath::Max(0.0, InLimits.MaxAgeSeconds), std::memory_order_relaxed);
}

FO3DSendQueueLimits FO3DSendQueue::GetLimits() const
{
	auto Load = [this](EO3DSendItemKind Kind)
	{
		const FKindState& State = GetKind(Kind);
		FO3DSendQueueKindLimits Limits;
		Limits.MaxItems = State.MaxItems.load(std::memory_order_relaxed);
		Limits.MaxBytes = State.MaxBytes.load(std::memory_order_relaxed);
		return Limits;
	};
	FO3DSendQueueLimits Limits;
	Limits.Mocap = Load(EO3DSendItemKind::Mocap);
	Limits.Audio = Load(EO3DSendItemKind::Audio);
	Limits.Control = Load(EO3DSendItemKind::Control);
	Limits.MocapOverflow = static_cast<EO3DMocapOverflow>(MocapOverflow.load(std::memory_order_relaxed));
	Limits.MaxAgeSeconds = MaxAgeSeconds.load(std::memory_order_relaxed);
	return Limits;
}

EO3DSendResult FO3DSendQueue::Enqueue(FO3DSendItem&& Item)
{
	const int64 Size = Item.Bytes.Num();
	if (Size <= 0 || static_cast<int32>(Item.Kind) >= O3DSendItemKindCount)
	{
		return EO3DSendResult::Invalid;
	}

	FKindState& State = GetKind(Item.Kind);
	// Mocap with DropOldest admits up to twice its soft cap; the consumer trims it back (see header).
	const bool bDoubles = Item.Kind == EO3DSendItemKind::Mocap
		&& static_cast<EO3DMocapOverflow>(MocapOverflow.load(std::memory_order_relaxed)) == EO3DMocapOverflow::DropOldest;
	const int32 MaxItems = O3DSendQueuePrivate::HardCap(State.MaxItems.load(std::memory_order_relaxed), bDoubles);
	const int64 MaxBytes = O3DSendQueuePrivate::HardCap(State.MaxBytes.load(std::memory_order_relaxed), bDoubles);

	// Reserve only what fits: concurrent producers can neither overshoot nor lose a count, and the
	// pending counters never read above a limit, not even for a moment (TRB-3).
	const bool bReservedItem = O3DSendQueuePrivate::TryReserve<int32>(State.Items, 1, MaxItems);
	if (!bReservedItem || !O3DSendQueuePrivate::TryReserve<int64>(State.Bytes, Size, MaxBytes))
	{
		if (bReservedItem)
		{
			State.Items.fetch_sub(1, std::memory_order_acq_rel);
		}
		State.Refused.fetch_add(1, std::memory_order_relaxed);
		return EO3DSendResult::DroppedBackpressure;
	}

	Item.EnqueueTimeSec = FPlatformTime::Seconds();
	Queue.Enqueue(MoveTemp(Item));
	State.Enqueued.fetch_add(1, std::memory_order_relaxed);
	WakeEvent->Trigger();
	return EO3DSendResult::Queued;
}

bool FO3DSendQueue::ShouldDiscard(const FO3DSendItem& Item, double NowSec) const
{
	if (Item.Kind == EO3DSendItemKind::Control)
	{
		return false;
	}

	if (Item.Kind == EO3DSendItemKind::Mocap
		&& static_cast<EO3DMocapOverflow>(MocapOverflow.load(std::memory_order_relaxed)) == EO3DMocapOverflow::DropOldest)
	{
		// Item is the oldest mocap item still queued and is still counted, so "over the soft cap"
		// includes it: discarding it brings the backlog one step back towards the cap.
		const FKindState& State = GetKind(EO3DSendItemKind::Mocap);
		const int32 SoftItems = State.MaxItems.load(std::memory_order_relaxed);
		const int64 SoftBytes = State.MaxBytes.load(std::memory_order_relaxed);
		if ((SoftItems > 0 && State.Items.load(std::memory_order_acquire) > SoftItems)
			|| (SoftBytes > 0 && State.Bytes.load(std::memory_order_acquire) > SoftBytes))
		{
			return true;
		}
	}

	const double MaxAge = MaxAgeSeconds.load(std::memory_order_relaxed);
	return MaxAge > 0.0 && (NowSec - Item.EnqueueTimeSec) > MaxAge;
}

bool FO3DSendQueue::Dequeue(FO3DSendItem& OutItem, double NowSec)
{
	while (Queue.Dequeue(OutItem))
	{
		FKindState& State = GetKind(OutItem.Kind);
		const bool bDiscard = ShouldDiscard(OutItem, NowSec);
		State.Items.fetch_sub(1, std::memory_order_acq_rel);
		State.Bytes.fetch_sub(OutItem.Bytes.Num(), std::memory_order_acq_rel);
		if (bDiscard)
		{
			State.Dropped.fetch_add(1, std::memory_order_relaxed);
			continue;
		}
		State.Dequeued.fetch_add(1, std::memory_order_relaxed);
		return true;
	}
	OutItem = FO3DSendItem();
	return false;
}

bool FO3DSendQueue::Dequeue(FO3DSendItem& OutItem)
{
	return Dequeue(OutItem, FPlatformTime::Seconds());
}

int32 FO3DSendQueue::Empty()
{
	int32 MocapDiscarded = 0;
	FO3DSendItem Item;
	while (Queue.Dequeue(Item))
	{
		FKindState& State = GetKind(Item.Kind);
		State.Items.fetch_sub(1, std::memory_order_acq_rel);
		State.Bytes.fetch_sub(Item.Bytes.Num(), std::memory_order_acq_rel);
		State.Dropped.fetch_add(1, std::memory_order_relaxed);
		MocapDiscarded += Item.Kind == EO3DSendItemKind::Mocap ? 1 : 0;
	}
	return MocapDiscarded;
}

FO3DSendQueueStats FO3DSendQueue::GetStats() const
{
	auto Read = [this](EO3DSendItemKind Kind)
	{
		const FKindState& State = GetKind(Kind);
		FO3DSendQueueKindStats Stats;
		// A producer's reservation can be visible for a moment before its rollback; never report
		// a negative or rolled-back count as a regression below zero.
		Stats.PendingItems = FMath::Max(0, State.Items.load(std::memory_order_acquire));
		Stats.PendingBytes = FMath::Max<int64>(0, State.Bytes.load(std::memory_order_acquire));
		Stats.Enqueued = State.Enqueued.load(std::memory_order_relaxed);
		Stats.Refused = State.Refused.load(std::memory_order_relaxed);
		Stats.Dropped = State.Dropped.load(std::memory_order_relaxed);
		Stats.Dequeued = State.Dequeued.load(std::memory_order_relaxed);
		return Stats;
	};
	FO3DSendQueueStats Stats;
	Stats.Mocap = Read(EO3DSendItemKind::Mocap);
	Stats.Audio = Read(EO3DSendItemKind::Audio);
	Stats.Control = Read(EO3DSendItemKind::Control);
	return Stats;
}

int32 FO3DSendQueue::GetPendingItems(EO3DSendItemKind Kind) const
{
	return FMath::Max(0, GetKind(Kind).Items.load(std::memory_order_acquire));
}

int64 FO3DSendQueue::GetPendingBytes() const
{
	int64 Total = 0;
	for (const FKindState& State : Kinds)
	{
		Total += FMath::Max<int64>(0, State.Bytes.load(std::memory_order_acquire));
	}
	return Total;
}

void FO3DSendQueue::Wake()
{
	WakeEvent->Trigger();
}

bool FO3DSendQueue::WaitForWork(uint32 WaitMs)
{
	return WakeEvent->Wait(WaitMs);
}
