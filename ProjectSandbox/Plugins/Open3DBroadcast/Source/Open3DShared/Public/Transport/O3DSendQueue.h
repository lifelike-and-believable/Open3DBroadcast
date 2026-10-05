// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/Event.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>

/*
 * The shared send queue (ADR 0007 item 7, WP-A1 step 4; TRB-3, TRB-14, TRB-38).
 *
 * One bounded multi-producer, single-consumer queue of typed items: mocap frames, encoded audio
 * and control envelopes (ADR 0011). Producers are SendSerialized/SendControl callers and audio
 * sinks, on any thread; the consumer is the transport worker (or, for Loopback, the receiver's
 * Poll). Each kind has its own limits and its own counters, so a kind can never take another
 * kind's room: mocap never pushes out audio or control, and a mocap flood never refuses a control
 * envelope (the "never dropped for mocap" rule of ADR 0007 item 7 and ADR 0011).
 *
 * Accounting is lock-free: a producer adds its item and bytes with fetch_add first and rolls them
 * back with fetch_sub when that crossed a limit, so concurrent producers can neither overshoot a
 * limit nor lose a count (TRB-3). The consumer subtracts after each dequeue. Near a limit, two
 * producers racing for the last slot may both be refused for a moment (each sees the other's
 * reservation); the queue never holds more than the limit.
 *
 * Drop policy:
 * - Mocap, EO3DMocapOverflow::DropOldest (ADR 0007 default): the limits are soft caps. Producers
 *   are refused (DroppedBackpressure) only at the hard cap, twice the soft cap. While the mocap
 *   backlog is over the soft cap, Dequeue discards the oldest mocap items (latency beats
 *   completeness for live mocap, ADR 0008 decision driver 4).
 * - Mocap, EO3DMocapOverflow::RefuseNewest: the limits are hard caps and nothing queued is ever
 *   discarded; for transports that promise ReliableOrdered delivery (Loopback).
 * - Audio and control: the limits are hard caps; a full kind refuses the newest item. Queued audio
 *   and control are never discarded to make room for anything.
 * - Age limit (TRB-14): with MaxAgeSeconds > 0, Dequeue discards mocap and audio items that waited
 *   longer. Control never expires here: its own TTL and snapshots handle staleness (ADR 0011).
 * A limit of 0 means "no limit" for that dimension.
 *
 * Threading: Enqueue, SetLimits, GetStats, GetPending*, Wake: any thread. Dequeue, WaitForWork and
 * Empty: the one consumer thread only (TQueue is MPSC).
 */

/** What a send queue item carries. */
enum class EO3DSendItemKind : uint8
{
	/** A serialized O3DS frame (SendSerialized). */
	Mocap = 0,
	/** An encoded audio frame from an audio sink. */
	Audio = 1,
	/** A control envelope (ADR 0011, SendControl). */
	Control = 2,
};

/** Number of EO3DSendItemKind values. */
inline constexpr int32 O3DSendItemKindCount = 3;

/** What to do with mocap at the queue's limit; see the file comment. */
enum class EO3DMocapOverflow : uint8
{
	DropOldest,
	RefuseNewest,
};

/** One queued item. The queue owns the bytes once Enqueue accepts the item. */
struct FO3DSendItem
{
	EO3DSendItemKind Kind = EO3DSendItemKind::Mocap;
	/** The bytes to transmit, in whatever form the transport puts on the wire. Never empty. */
	TArray<uint8> Bytes;
	/** Subject of a mocap frame or audio frame; may be empty. */
	FString Subject;
	/** Sender-clock capture time (FO3DSendPayload::CaptureTimeSec, or the audio frame's timestamp). */
	double CaptureTimeSec = 0.0;
	/** Set by Enqueue: FPlatformTime::Seconds() when the item was accepted. Drives the age limit. */
	double EnqueueTimeSec = 0.0;
	/** FO3DSendPayload::bFullSync for mocap; false otherwise. */
	bool bFullSync = false;

	static FO3DSendItem MakeMocap(TArray<uint8>&& InBytes, FString InSubject, double InCaptureTimeSec, bool bInFullSync = false)
	{
		FO3DSendItem Item;
		Item.Kind = EO3DSendItemKind::Mocap;
		Item.Bytes = MoveTemp(InBytes);
		Item.Subject = MoveTemp(InSubject);
		Item.CaptureTimeSec = InCaptureTimeSec;
		Item.bFullSync = bInFullSync;
		return Item;
	}

	static FO3DSendItem MakeAudio(TArray<uint8>&& InBytes, FString InSubject, double InCaptureTimeSec)
	{
		FO3DSendItem Item;
		Item.Kind = EO3DSendItemKind::Audio;
		Item.Bytes = MoveTemp(InBytes);
		Item.Subject = MoveTemp(InSubject);
		Item.CaptureTimeSec = InCaptureTimeSec;
		return Item;
	}

	static FO3DSendItem MakeControl(TArray<uint8>&& InEnvelope)
	{
		FO3DSendItem Item;
		Item.Kind = EO3DSendItemKind::Control;
		Item.Bytes = MoveTemp(InEnvelope);
		return Item;
	}
};

/** Limits of one item kind. 0 means no limit. */
struct FO3DSendQueueKindLimits
{
	int32 MaxItems = 0;
	int64 MaxBytes = 0;
};

/** Control envelopes that may wait before SendControl is refused (the publisher retries; ADR 0011). */
inline constexpr int32 O3DSendQueueDefaultMaxControlItems = 1024;

/** The queue's limits and policies. */
struct FO3DSendQueueLimits
{
	/** Soft caps with DropOldest, hard caps with RefuseNewest. */
	FO3DSendQueueKindLimits Mocap;
	/** Hard caps. */
	FO3DSendQueueKindLimits Audio;
	/** Hard caps. Kept separate from mocap, so control always has room of its own. */
	FO3DSendQueueKindLimits Control{ O3DSendQueueDefaultMaxControlItems, 0 };
	EO3DMocapOverflow MocapOverflow = EO3DMocapOverflow::DropOldest;
	/** Mocap and audio older than this are discarded at Dequeue (TRB-14). 0 disables. */
	double MaxAgeSeconds = 0.0;
};

/** Counters of one kind. A snapshot; fields are read one by one, so they may be a moment apart. */
struct FO3DSendQueueKindStats
{
	/** Items and bytes waiting now. */
	int32 PendingItems = 0;
	int64 PendingBytes = 0;
	/** Items Enqueue accepted. */
	int64 Enqueued = 0;
	/** Items Enqueue refused (DroppedBackpressure). */
	int64 Refused = 0;
	/** Accepted items discarded later: oldest-first mocap eviction, the age limit, or Empty(). */
	int64 Dropped = 0;
	/** Items Dequeue handed to the consumer. */
	int64 Dequeued = 0;
};

/** Snapshot of every kind's counters. */
struct FO3DSendQueueStats
{
	FO3DSendQueueKindStats Mocap;
	FO3DSendQueueKindStats Audio;
	FO3DSendQueueKindStats Control;

	const FO3DSendQueueKindStats& Get(EO3DSendItemKind Kind) const
	{
		return Kind == EO3DSendItemKind::Mocap ? Mocap : (Kind == EO3DSendItemKind::Audio ? Audio : Control);
	}

	int32 GetPendingItems() const { return Mocap.PendingItems + Audio.PendingItems + Control.PendingItems; }
	int64 GetPendingBytes() const { return Mocap.PendingBytes + Audio.PendingBytes + Control.PendingBytes; }
};

class OPEN3DSHARED_API FO3DSendQueue
{
public:
	explicit FO3DSendQueue(const FO3DSendQueueLimits& InLimits = FO3DSendQueueLimits());
	~FO3DSendQueue();

	FO3DSendQueue(const FO3DSendQueue&) = delete;
	FO3DSendQueue& operator=(const FO3DSendQueue&) = delete;

	/** Any thread. Takes effect for later Enqueue and Dequeue calls; queued items stay. */
	void SetLimits(const FO3DSendQueueLimits& InLimits);
	FO3DSendQueueLimits GetLimits() const;

	/**
	 * Any thread; never blocks. Queued (the item is moved in and the consumer woken),
	 * DroppedBackpressure (the item's kind is at its limit; Item is left untouched, so the caller
	 * may retry it) or Invalid (empty bytes).
	 */
	EO3DSendResult Enqueue(FO3DSendItem&& Item);

	/**
	 * Consumer thread only. The oldest item that survives the drop policy, or false when the
	 * queue is empty. NowSec is the time the age limit is checked against.
	 */
	bool Dequeue(FO3DSendItem& OutItem, double NowSec);
	bool Dequeue(FO3DSendItem& OutItem);

	/**
	 * Discards every queued item and counts them as Dropped. Consumer thread only, or while no
	 * consumer runs. Returns the number of mocap items discarded (frames a sender reports as dropped).
	 */
	int32 Empty();

	/** Any thread. */
	FO3DSendQueueStats GetStats() const;
	int32 GetPendingItems(EO3DSendItemKind Kind) const;
	int64 GetPendingBytes() const;

	/** Wakes the consumer. Any thread, for as long as the caller holds the queue. */
	void Wake();

	/** Consumer thread: waits up to WaitMs for an Enqueue or a Wake. True when woken. */
	bool WaitForWork(uint32 WaitMs);

private:
	struct FKindState
	{
		std::atomic<int32> Items{ 0 };
		std::atomic<int64> Bytes{ 0 };
		std::atomic<int64> Enqueued{ 0 };
		std::atomic<int64> Refused{ 0 };
		std::atomic<int64> Dropped{ 0 };
		std::atomic<int64> Dequeued{ 0 };
		std::atomic<int32> MaxItems{ 0 };
		std::atomic<int64> MaxBytes{ 0 };
	};

	FKindState& GetKind(EO3DSendItemKind Kind) { return Kinds[static_cast<int32>(Kind)]; }
	const FKindState& GetKind(EO3DSendItemKind Kind) const { return Kinds[static_cast<int32>(Kind)]; }

	/** True when Dequeue should discard this item instead of handing it out. */
	bool ShouldDiscard(const FO3DSendItem& Item, double NowSec) const;

	FKindState Kinds[O3DSendItemKindCount];
	std::atomic<uint8> MocapOverflow{ static_cast<uint8>(EO3DMocapOverflow::DropOldest) };
	std::atomic<double> MaxAgeSeconds{ 0.0 };
	TQueue<FO3DSendItem, EQueueMode::Mpsc> Queue;
	FEventRef WakeEvent{ EEventMode::AutoReset };
};
