// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// FO3DSendQueue (ADR 0007 item 7, WP-A1 step 4; TRB-3, TRB-14, ADR 0011): limits per kind, the
// mocap drop policies, control and audio never dropped for mocap, the age limit, FIFO order across
// kinds, and lock-free accounting under several producers and a concurrent consumer.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Transport/O3DSendQueue.h"

#include <atomic>

namespace O3DSendQueueTests
{
	TArray<uint8> MakeBytes(int32 Size, uint8 Seed = 0)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Bytes[Index] = static_cast<uint8>(Seed + Index);
		}
		return Bytes;
	}

	FO3DSendItem MocapItem(int32 Size, uint8 Seed = 0) { return FO3DSendItem::MakeMocap(MakeBytes(Size, Seed), TEXT("Subject"), 0.0); }
	FO3DSendItem AudioItem(int32 Size, uint8 Seed = 0) { return FO3DSendItem::MakeAudio(MakeBytes(Size, Seed), TEXT("Subject"), 0.0); }
	FO3DSendItem ControlItem(int32 Size, uint8 Seed = 0) { return FO3DSendItem::MakeControl(MakeBytes(Size, Seed)); }

	/** Runs a body on its own thread; joined by Join or the destructor. */
	class FSendQueueTestThread final : public FRunnable
	{
	public:
		explicit FSendQueueTestThread(TFunction<void()> InBody)
			: Body(MoveTemp(InBody))
		{
			Thread = FRunnableThread::Create(this, TEXT("O3D_SendQueueTestThread"));
		}
		virtual ~FSendQueueTestThread() override { Join(); }
		void Join()
		{
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
		}
		virtual uint32 Run() override
		{
			Body();
			return 0;
		}

	private:
		TFunction<void()> Body;
		FRunnableThread* Thread = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueInvalidTest, "Open3DBroadcast.Shared.SendQueue.EmptyItemIsInvalid", O3DB_TEST_FLAGS)
bool FO3DSendQueueInvalidTest::RunTest(const FString& Parameters)
{
	FO3DSendQueue Queue;
	FO3DSendItem Empty = FO3DSendItem::MakeMocap(TArray<uint8>(), TEXT("S"), 0.0);
	TestTrue(TEXT("Empty bytes are Invalid"), Queue.Enqueue(MoveTemp(Empty)) == EO3DSendResult::Invalid);
	TestEqual(TEXT("Nothing pending"), Queue.GetStats().GetPendingItems(), 0);
	FO3DSendItem Out;
	TestFalse(TEXT("Dequeue from an empty queue"), Queue.Dequeue(Out));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueRefuseNewestTest, "Open3DBroadcast.Shared.SendQueue.RefuseNewestAtItemLimit", O3DB_TEST_FLAGS)
bool FO3DSendQueueRefuseNewestTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	FO3DSendQueueLimits Limits;
	Limits.Mocap.MaxItems = 2;
	Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
	FO3DSendQueue Queue(Limits);

	TestTrue(TEXT("1st queued"), Queue.Enqueue(MocapItem(10, 1)) == EO3DSendResult::Queued);
	TestTrue(TEXT("2nd queued"), Queue.Enqueue(MocapItem(10, 2)) == EO3DSendResult::Queued);
	FO3DSendItem Third = MocapItem(10, 3);
	TestTrue(TEXT("3rd refused at the hard cap"), Queue.Enqueue(MoveTemp(Third)) == EO3DSendResult::DroppedBackpressure);
	TestEqual(TEXT("A refused item is left untouched for a retry"), Third.Bytes.Num(), 10);

	const FO3DSendQueueStats Stats = Queue.GetStats();
	TestEqual(TEXT("Pending mocap"), Stats.Mocap.PendingItems, 2);
	TestEqual(TEXT("Pending bytes"), Stats.Mocap.PendingBytes, static_cast<int64>(20));
	TestEqual(TEXT("Refused"), Stats.Mocap.Refused, static_cast<int64>(1));

	FO3DSendItem Out;
	TestTrue(TEXT("Oldest first"), Queue.Dequeue(Out) && Out.Bytes[0] == 1);
	TestTrue(TEXT("Nothing discarded with RefuseNewest"), Queue.Dequeue(Out) && Out.Bytes[0] == 2);
	TestTrue(TEXT("Room again after a dequeue"), Queue.Enqueue(MocapItem(10, 4)) == EO3DSendResult::Queued);
	TestEqual(TEXT("No drops"), Queue.GetStats().Mocap.Dropped, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueByteLimitTest, "Open3DBroadcast.Shared.SendQueue.ByteLimit", O3DB_TEST_FLAGS)
bool FO3DSendQueueByteLimitTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	FO3DSendQueueLimits Limits;
	Limits.Audio.MaxBytes = 100;
	FO3DSendQueue Queue(Limits);

	TestTrue(TEXT("60 bytes fit"), Queue.Enqueue(AudioItem(60)) == EO3DSendResult::Queued);
	TestTrue(TEXT("41 more would exceed 100"), Queue.Enqueue(AudioItem(41)) == EO3DSendResult::DroppedBackpressure);
	TestTrue(TEXT("40 more reach exactly 100"), Queue.Enqueue(AudioItem(40)) == EO3DSendResult::Queued);
	TestTrue(TEXT("An item larger than the cap is always refused"), Queue.Enqueue(AudioItem(101)) == EO3DSendResult::DroppedBackpressure);
	TestEqual(TEXT("Pending bytes never pass the cap"), Queue.GetStats().Audio.PendingBytes, static_cast<int64>(100));
	TestEqual(TEXT("Refused audio"), Queue.GetStats().Audio.Refused, static_cast<int64>(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueDropOldestTest, "Open3DBroadcast.Shared.SendQueue.DropOldestSoftAndHardCap", O3DB_TEST_FLAGS)
bool FO3DSendQueueDropOldestTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	FO3DSendQueueLimits Limits;
	Limits.Mocap.MaxItems = 2; // soft cap; hard cap 4 (ADR 0007 item 7)
	Limits.MocapOverflow = EO3DMocapOverflow::DropOldest;
	FO3DSendQueue Queue(Limits);

	for (uint8 Index = 1; Index <= 4; ++Index)
	{
		TestTrue(*FString::Printf(TEXT("Frame %d accepted below the hard cap"), Index), Queue.Enqueue(MocapItem(8, Index)) == EO3DSendResult::Queued);
	}
	TestTrue(TEXT("5th refused at the hard cap (twice the soft cap)"), Queue.Enqueue(MocapItem(8, 5)) == EO3DSendResult::DroppedBackpressure);

	FO3DSendItem Out;
	TestTrue(TEXT("Dequeue returns a frame"), Queue.Dequeue(Out));
	TestEqual(TEXT("The two oldest were dropped: the backlog is back at the soft cap"), static_cast<int32>(Out.Bytes[0]), 3);
	TestTrue(TEXT("Next frame"), Queue.Dequeue(Out) && Out.Bytes[0] == 4);
	TestFalse(TEXT("Queue empty"), Queue.Dequeue(Out));

	const FO3DSendQueueStats Stats = Queue.GetStats();
	TestEqual(TEXT("Dropped oldest"), Stats.Mocap.Dropped, static_cast<int64>(2));
	TestEqual(TEXT("Dequeued"), Stats.Mocap.Dequeued, static_cast<int64>(2));
	TestEqual(TEXT("Refused"), Stats.Mocap.Refused, static_cast<int64>(1));
	TestEqual(TEXT("Nothing pending"), Stats.Mocap.PendingItems, 0);
	TestEqual(TEXT("No pending bytes"), Stats.Mocap.PendingBytes, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueControlNeverDroppedTest, "Open3DBroadcast.Shared.SendQueue.ControlNeverDroppedForMocap", O3DB_TEST_FLAGS)
bool FO3DSendQueueControlNeverDroppedTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	{
		// A full mocap kind does not refuse control or audio.
		FO3DSendQueueLimits Limits;
		Limits.Mocap.MaxItems = 1;
		Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
		FO3DSendQueue Queue(Limits);
		TestTrue(TEXT("Mocap fills its limit"), Queue.Enqueue(MocapItem(4)) == EO3DSendResult::Queued);
		TestTrue(TEXT("Mocap refused"), Queue.Enqueue(MocapItem(4)) == EO3DSendResult::DroppedBackpressure);
		TestTrue(TEXT("Control still accepted"), Queue.Enqueue(ControlItem(4)) == EO3DSendResult::Queued);
		TestTrue(TEXT("Audio still accepted"), Queue.Enqueue(AudioItem(4)) == EO3DSendResult::Queued);
	}
	{
		// Eviction of old mocap never takes the control or audio items between the frames.
		FO3DSendQueueLimits Limits;
		Limits.Mocap.MaxItems = 1;
		Limits.MocapOverflow = EO3DMocapOverflow::DropOldest;
		FO3DSendQueue Queue(Limits);
		Queue.Enqueue(MocapItem(4, 1));
		Queue.Enqueue(ControlItem(4, 10));
		Queue.Enqueue(AudioItem(4, 20));
		Queue.Enqueue(MocapItem(4, 2));

		TArray<EO3DSendItemKind> Kinds;
		TArray<uint8> FirstBytes;
		FO3DSendItem Out;
		while (Queue.Dequeue(Out))
		{
			Kinds.Add(Out.Kind);
			FirstBytes.Add(Out.Bytes[0]);
		}
		TestEqual(TEXT("Three items survive"), Kinds.Num(), 3);
		TestTrue(TEXT("The oldest frame was dropped; control and audio kept, in order"),
			Kinds.Num() == 3 && Kinds[0] == EO3DSendItemKind::Control && Kinds[1] == EO3DSendItemKind::Audio && Kinds[2] == EO3DSendItemKind::Mocap
			&& FirstBytes[2] == 2);
		TestEqual(TEXT("Control dropped"), Queue.GetStats().Control.Dropped, static_cast<int64>(0));
		TestEqual(TEXT("Audio dropped"), Queue.GetStats().Audio.Dropped, static_cast<int64>(0));
	}
	{
		// Control has its own cap (the publisher retries past it, ADR 0011).
		FO3DSendQueueLimits Limits;
		Limits.Control.MaxItems = 3;
		FO3DSendQueue Queue(Limits);
		int32 Accepted = 0;
		while (Queue.Enqueue(ControlItem(4)) == EO3DSendResult::Queued && Accepted < 100)
		{
			++Accepted;
		}
		TestEqual(TEXT("Control accepted up to its own cap"), Accepted, 3);
		TestEqual(TEXT("Default control cap"), FO3DSendQueueLimits().Control.MaxItems, O3DSendQueueDefaultMaxControlItems);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueAgeLimitTest, "Open3DBroadcast.Shared.SendQueue.AgeLimit", O3DB_TEST_FLAGS)
bool FO3DSendQueueAgeLimitTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	FO3DSendQueueLimits Limits;
	Limits.MaxAgeSeconds = 0.5;
	FO3DSendQueue Queue(Limits);
	Queue.Enqueue(MocapItem(4, 1));
	Queue.Enqueue(AudioItem(4, 2));
	Queue.Enqueue(ControlItem(4, 3));

	FO3DSendItem Out;
	const double Later = FPlatformTime::Seconds() + 10.0;
	TestTrue(TEXT("Only control survives the age limit"), Queue.Dequeue(Out, Later) && Out.Kind == EO3DSendItemKind::Control);
	TestFalse(TEXT("Nothing else"), Queue.Dequeue(Out, Later));
	TestEqual(TEXT("Expired mocap counted"), Queue.GetStats().Mocap.Dropped, static_cast<int64>(1));
	TestEqual(TEXT("Expired audio counted"), Queue.GetStats().Audio.Dropped, static_cast<int64>(1));

	Queue.Enqueue(MocapItem(4, 4));
	TestTrue(TEXT("A fresh frame is handed out"), Queue.Dequeue(Out) && Out.Bytes[0] == 4);
	TestTrue(TEXT("Enqueue stamps the time"), Out.EnqueueTimeSec > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueOrderTest, "Open3DBroadcast.Shared.SendQueue.FifoAcrossKinds", O3DB_TEST_FLAGS)
bool FO3DSendQueueOrderTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	FO3DSendQueue Queue;
	for (uint8 Index = 0; Index < 30; ++Index)
	{
		FO3DSendItem Item = (Index % 3 == 0) ? MocapItem(2, Index) : ((Index % 3 == 1) ? AudioItem(2, Index) : ControlItem(2, Index));
		Queue.Enqueue(MoveTemp(Item));
	}
	FO3DSendItem Out;
	bool bInOrder = true;
	for (uint8 Index = 0; Index < 30; ++Index)
	{
		bInOrder &= Queue.Dequeue(Out) && Out.Bytes[0] == Index && static_cast<int32>(Out.Kind) == Index % 3;
	}
	TestTrue(TEXT("Items leave in the order they came, whatever their kind"), bInOrder);

	FO3DSendItem WithMeta = FO3DSendItem::MakeMocap(MakeBytes(3), TEXT("Hero"), 12.5, /*bInFullSync=*/true);
	Queue.Enqueue(MoveTemp(WithMeta));
	TestTrue(TEXT("Subject, capture time and full-sync flag travel with the item"),
		Queue.Dequeue(Out) && Out.Subject == TEXT("Hero") && Out.CaptureTimeSec == 12.5 && Out.bFullSync);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueEmptyAndLimitsTest, "Open3DBroadcast.Shared.SendQueue.EmptyAndRuntimeLimits", O3DB_TEST_FLAGS)
bool FO3DSendQueueEmptyAndLimitsTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	FO3DSendQueue Queue;
	Queue.Enqueue(MocapItem(4));
	Queue.Enqueue(MocapItem(4));
	Queue.Enqueue(AudioItem(4));
	Queue.Enqueue(ControlItem(4));
	TestEqual(TEXT("Empty reports the discarded frames"), Queue.Empty(), 2);
	TestEqual(TEXT("Nothing pending"), Queue.GetStats().GetPendingItems(), 0);
	TestEqual(TEXT("No pending bytes"), Queue.GetPendingBytes(), static_cast<int64>(0));
	TestEqual(TEXT("Every discarded item counted"), Queue.GetStats().Mocap.Dropped + Queue.GetStats().Audio.Dropped + Queue.GetStats().Control.Dropped, static_cast<int64>(4));

	FO3DSendQueueLimits Limits = Queue.GetLimits();
	Limits.Mocap.MaxItems = 1;
	Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
	Queue.SetLimits(Limits);
	TestEqual(TEXT("Limits read back"), Queue.GetLimits().Mocap.MaxItems, 1);
	TestTrue(TEXT("New limit applies"), Queue.Enqueue(MocapItem(4)) == EO3DSendResult::Queued && Queue.Enqueue(MocapItem(4)) == EO3DSendResult::DroppedBackpressure);

	TestTrue(TEXT("An Enqueue wakes the consumer"), Queue.WaitForWork(0) || Queue.WaitForWork(1000));
	Queue.Wake();
	TestTrue(TEXT("Wake wakes the consumer"), Queue.WaitForWork(1000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueConcurrentAccountingTest, "Open3DBroadcast.Shared.SendQueue.ConcurrentAccounting", O3DB_TEST_FLAGS)
bool FO3DSendQueueConcurrentAccountingTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	static constexpr int32 NumProducers = 4;
	static constexpr int32 ItemsPerProducer = 5000;

	{
		// Producers race a live consumer: every accepted byte comes out exactly once (TRB-3).
		FO3DSendQueueLimits Limits;
		Limits.Mocap.MaxBytes = 4096;
		Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
		Limits.Audio.MaxBytes = 4096;
		Limits.Control.MaxItems = 64;
		FO3DSendQueue Queue(Limits);

		std::atomic<int64> AcceptedBytes{ 0 };
		std::atomic<int64> AcceptedItems{ 0 };
		std::atomic<int32> ProducersDone{ 0 };
		int64 ConsumedBytes = 0;
		int64 ConsumedItems = 0;
		bool bOverCap = false;

		TArray<TUniquePtr<FSendQueueTestThread>> Producers;
		for (int32 Producer = 0; Producer < NumProducers; ++Producer)
		{
			Producers.Add(MakeUnique<FSendQueueTestThread>([&Queue, &AcceptedBytes, &AcceptedItems, &ProducersDone, Producer]()
			{
				for (int32 Index = 0; Index < ItemsPerProducer; ++Index)
				{
					const int32 Size = 1 + ((Index * 7 + Producer * 13) % 200);
					const int32 Kind = (Index + Producer) % 3;
					FO3DSendItem Item = Kind == 0 ? MocapItem(Size) : (Kind == 1 ? AudioItem(Size) : ControlItem(Size));
					if (Queue.Enqueue(MoveTemp(Item)) == EO3DSendResult::Queued)
					{
						AcceptedBytes.fetch_add(Size);
						AcceptedItems.fetch_add(1);
					}
				}
				ProducersDone.fetch_add(1);
			}));
		}

		FO3DSendItem Out;
		while (ProducersDone.load() < NumProducers || Queue.GetStats().GetPendingItems() > 0)
		{
			const FO3DSendQueueStats Stats = Queue.GetStats();
			bOverCap |= Stats.Mocap.PendingBytes > 4096 || Stats.Audio.PendingBytes > 4096 || Stats.Control.PendingItems > 64;
			if (Queue.Dequeue(Out))
			{
				ConsumedBytes += Out.Bytes.Num();
				++ConsumedItems;
			}
		}
		for (TUniquePtr<FSendQueueTestThread>& Producer : Producers)
		{
			Producer->Join();
		}
		while (Queue.Dequeue(Out))
		{
			ConsumedBytes += Out.Bytes.Num();
			++ConsumedItems;
		}

		const FO3DSendQueueStats Stats = Queue.GetStats();
		TestFalse(TEXT("No kind ever went over its limit"), bOverCap);
		TestEqual(TEXT("Every accepted item came out once"), ConsumedItems, AcceptedItems.load());
		TestEqual(TEXT("Every accepted byte came out once"), ConsumedBytes, AcceptedBytes.load());
		TestEqual(TEXT("Pending bytes return to zero"), Stats.GetPendingBytes(), static_cast<int64>(0));
		TestEqual(TEXT("Pending items return to zero"), Stats.GetPendingItems(), 0);
		TestEqual(TEXT("Accepted plus refused is every attempt"),
			Stats.Mocap.Enqueued + Stats.Audio.Enqueued + Stats.Control.Enqueued + Stats.Mocap.Refused + Stats.Audio.Refused + Stats.Control.Refused,
			static_cast<int64>(NumProducers * ItemsPerProducer));
	}

	{
		// No consumer: racing producers never overshoot an item limit.
		FO3DSendQueueLimits Limits;
		Limits.Mocap.MaxItems = 100;
		Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
		FO3DSendQueue Queue(Limits);
		std::atomic<int32> Accepted{ 0 };
		{
			TArray<TUniquePtr<FSendQueueTestThread>> Producers;
			for (int32 Producer = 0; Producer < NumProducers; ++Producer)
			{
				Producers.Add(MakeUnique<FSendQueueTestThread>([&Queue, &Accepted]()
				{
					for (int32 Index = 0; Index < 1000; ++Index)
					{
						if (Queue.Enqueue(MocapItem(16)) == EO3DSendResult::Queued)
						{
							Accepted.fetch_add(1);
						}
					}
				}));
			}
		}
		const FO3DSendQueueStats Stats = Queue.GetStats();
		TestTrue(*FString::Printf(TEXT("At most the limit was accepted (%d)"), Accepted.load()), Accepted.load() <= 100 && Accepted.load() > 0);
		TestEqual(TEXT("Pending equals accepted"), Stats.Mocap.PendingItems, Accepted.load());
		TestEqual(TEXT("Pending bytes equal accepted bytes"), Stats.Mocap.PendingBytes, static_cast<int64>(Accepted.load()) * 16);
	}
	return true;
}

// WP-R1 (mid-project review TR-1): a full sync is what a receiver resyncs from (ADR 0005), and the
// sender already counts it as sent once Enqueue says Queued. The age limit and the oldest-first
// eviction must not discard it, or receivers hold the subject until the next periodic one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSendQueueKeepsFullSyncTest, "Open3DBroadcast.Shared.SendQueue.NeverDiscardsAFullSync", O3DB_TEST_FLAGS)
bool FO3DSendQueueKeepsFullSyncTest::RunTest(const FString& Parameters)
{
	using namespace O3DSendQueueTests;
	{
		FO3DSendQueueLimits Limits;
		Limits.MaxAgeSeconds = 0.5;
		FO3DSendQueue Queue(Limits);
		Queue.Enqueue(FO3DSendItem::MakeMocap(MakeBytes(4, 1), TEXT("Subject"), 0.0, true));
		Queue.Enqueue(MocapItem(4, 2));

		FO3DSendItem Out;
		const double Later = FPlatformTime::Seconds() + 10.0;
		TestTrue(TEXT("An old full sync is still sent"), Queue.Dequeue(Out, Later) && Out.bFullSync && Out.Bytes[0] == 1);
		TestFalse(TEXT("An old update is not"), Queue.Dequeue(Out, Later));
	}
	{
		FO3DSendQueueLimits Limits;
		Limits.Mocap.MaxItems = 1; // soft cap; DropOldest
		FO3DSendQueue Queue(Limits);
		Queue.Enqueue(FO3DSendItem::MakeMocap(MakeBytes(4, 1), TEXT("Subject"), 0.0, true));
		Queue.Enqueue(MocapItem(4, 2));

		FO3DSendItem Out;
		TestTrue(TEXT("The oldest frame over the soft cap is kept when it is a full sync"), Queue.Dequeue(Out) && Out.bFullSync && Out.Bytes[0] == 1);
		TestTrue(TEXT("And the update after it follows"), Queue.Dequeue(Out) && Out.Bytes[0] == 2);
	}
	{
		// A discarded update is reported once, so the sender can ask for a full sync.
		FO3DSendQueueLimits Limits;
		Limits.MaxAgeSeconds = 0.5;
		FO3DSendQueue Queue(Limits);
		Queue.Enqueue(MocapItem(4, 1));
		TestFalse(TEXT("Nothing discarded yet"), Queue.ConsumeMocapDiscarded());
		FO3DSendItem Out;
		Queue.Dequeue(Out, FPlatformTime::Seconds() + 10.0);
		TestTrue(TEXT("The expired update is reported"), Queue.ConsumeMocapDiscarded());
		TestFalse(TEXT("Once"), Queue.ConsumeMocapDiscarded());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
