// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
//
// WP-S10 (SHR-3, SHR-17, SHR-26): transport metrics have stable addresses and are updated
// through per-transport handles without a lock; readers get snapshots; rolling averages and
// peaks are updated with compare-exchange so concurrent updates are not lost.
// The registry under test is a local instance, so the process-wide metrics are not touched.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "O3DPerformanceMetrics.h"
#include "Templates/UniquePtr.h"

#include <atomic>

namespace O3DPerformanceMetricsTests
{
	// Namespace scope so the worker lambdas can use them without capturing.
	constexpr int32 NumWriters = 8;
	constexpr int32 NamesPerWriter = 64;
	constexpr int32 UpdatesPerName = 200;
	constexpr int32 SharedUpdatesPerWriter = 5000;
	constexpr int32 NumThreads = 8;
	constexpr int32 UpdatesPerThread = 100; // 800 halvings: 2^-800 is still a normal double

	class FWorker final : public FRunnable
	{
	public:
		explicit FWorker(TFunction<void()> InBody)
			: Body(MoveTemp(InBody))
		{
			Thread = FRunnableThread::Create(this, TEXT("O3D_MetricsTestThread"));
		}

		virtual ~FWorker() override
		{
			Join();
		}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DMetricsConcurrentRegistrationTest, "Open3DBroadcast.Shared.Metrics.ConcurrentRegistrationAndUpdates", O3DB_TEST_FLAGS)
bool FO3DMetricsConcurrentRegistrationTest::RunTest(const FString& Parameters)
{
	using namespace O3DPerformanceMetricsTests;

	// Before SHR-3, registering a new name could reallocate the array that other threads were
	// writing through raw pointers. Here eight writers each register 64 new names while also
	// updating one shared entry, and a reader takes snapshots the whole time.

	FO3DTransportMetricsRegistry Registry;
	const FO3DTransportMetricsRef Shared = Registry.Acquire(TEXT("Shared"));
	const FO3DTransportMetrics* const SharedAddress = &Shared.Get();

	std::atomic<bool> bStopReader{false};
	std::atomic<int32> Snapshots{0};
	std::atomic<int32> UnsortedSnapshots{0};
	std::atomic<int32> AddressChanges{0};
	{
		FWorker Reader([&]()
		{
			while (!bStopReader.load())
			{
				const TArray<FO3DTransportMetricsSnapshot> Snapshot = Registry.Snapshot();
				for (int32 Index = 1; Index < Snapshot.Num(); ++Index)
				{
					if (Snapshot[Index - 1].TransportName.Compare(Snapshot[Index].TransportName) > 0)
					{
						UnsortedSnapshots.fetch_add(1);
					}
				}
				const TSharedPtr<FO3DTransportMetrics, ESPMode::ThreadSafe> Found = Registry.Find(TEXT("Shared"));
				if (!Found.IsValid() || Found.Get() != SharedAddress)
				{
					AddressChanges.fetch_add(1);
				}
				Snapshots.fetch_add(1);
				FPlatformProcess::YieldThread();
			}
		});

		TArray<TUniquePtr<FWorker>> Writers;
		for (int32 Writer = 0; Writer < NumWriters; ++Writer)
		{
			Writers.Add(MakeUnique<FWorker>([&Registry, &Shared, Writer]()
			{
				for (int32 Name = 0; Name < NamesPerWriter; ++Name)
				{
					const FName TransportName(*FString::Printf(TEXT("W%d_T%d"), Writer, Name));
					const FO3DTransportMetricsRef Handle = Registry.Acquire(TransportName);
					for (int32 Update = 0; Update < UpdatesPerName; ++Update)
					{
						Handle->RecordFrameSent(3);
						Handle->UpdatePendingFrames(Update);
					}
					// The same name always resolves to the same entry.
					Registry.Acquire(TransportName)->RecordFrameReceived(1);
				}
				for (int32 Update = 0; Update < SharedUpdatesPerWriter; ++Update)
				{
					Shared->RecordFrameSent(1);
					Shared->UpdatePendingFrames(Writer * SharedUpdatesPerWriter + Update);
					if (Update % 100 == 0)
					{
						Shared->RecordError(true);
					}
				}
			}));
		}
		for (TUniquePtr<FWorker>& Writer : Writers)
		{
			Writer->Join();
		}
		bStopReader.store(true);
		Reader.Join();
	}

	TestEqual(TEXT("Every name registered once"), Registry.Num(), NumWriters * NamesPerWriter + 1);
	TestEqual(TEXT("Snapshots are sorted by name"), UnsortedSnapshots.load(), 0);
	TestEqual(TEXT("The shared entry never moved"), AddressChanges.load(), 0);
	TestTrue(TEXT("The reader ran concurrently"), Snapshots.load() > 0);

	const TArray<FO3DTransportMetricsSnapshot> Final = Registry.Snapshot();
	TestEqual(TEXT("Snapshot has every entry"), Final.Num(), NumWriters * NamesPerWriter + 1);
	int32 WrongEntries = 0;
	for (const FO3DTransportMetricsSnapshot& Entry : Final)
	{
		if (Entry.TransportName == FName(TEXT("Shared")))
		{
			TestEqual(TEXT("Shared: no lost frame updates"), Entry.FramesSent, static_cast<uint64>(NumWriters * SharedUpdatesPerWriter));
			TestEqual(TEXT("Shared: bytes"), Entry.BytesSent, static_cast<uint64>(NumWriters * SharedUpdatesPerWriter));
			TestEqual(TEXT("Shared: errors"), Entry.SendErrors, static_cast<uint64>(NumWriters * (SharedUpdatesPerWriter / 100)));
			TestEqual(TEXT("Shared: the peak is the largest value any thread reported"), Entry.MaxPendingFrames, NumWriters * SharedUpdatesPerWriter - 1);
			continue;
		}
		const bool bRight = Entry.FramesSent == static_cast<uint64>(UpdatesPerName)
			&& Entry.BytesSent == static_cast<uint64>(UpdatesPerName * 3)
			&& Entry.FramesReceived == static_cast<uint64>(1)
			&& Entry.MaxPendingFrames == UpdatesPerName - 1;
		WrongEntries += bRight ? 0 : 1;
	}
	TestEqual(TEXT("Per-writer entries hold exactly their own updates"), WrongEntries, 0);

	Registry.ResetCounters();
	TestEqual(TEXT("Reset zeroes counters through existing handles"), Shared->FramesSent.load(), static_cast<uint64>(0));
	TestTrue(TEXT("Handles stay valid after a reset"), Registry.Find(TEXT("Shared")).Get() == SharedAddress);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DMetricsAtomicUpdatesTest, "Open3DBroadcast.Shared.Metrics.AtomicMaxAndAverage", O3DB_TEST_FLAGS)
bool FO3DMetricsAtomicUpdatesTest::RunTest(const FString& Parameters)
{
	using namespace O3DPerformanceMetricsTests;

	// SHR-26: with a load-then-store update, concurrent writers lose updates and the peak can
	// move down. Alpha 0.5 with samples of 0 halves the average exactly once per update, so
	// after N updates it must be exactly 2^-N; any lost update leaves a larger value.

	std::atomic<double> Average{1.0};
	std::atomic<double> Peak{0.0};
	std::atomic<int32> IntPeak{0};
	{
		TArray<TUniquePtr<FWorker>> Threads;
		for (int32 Thread = 0; Thread < NumThreads; ++Thread)
		{
			Threads.Add(MakeUnique<FWorker>([&Average, &Peak, &IntPeak, Thread]()
			{
				for (int32 Update = 0; Update < UpdatesPerThread; ++Update)
				{
					O3DMetrics::AtomicUpdateEma(Average, 0.0, 0.5);
					const int32 Value = Update * NumThreads + Thread;
					O3DMetrics::AtomicStoreMax(Peak, static_cast<double>(Value));
					O3DMetrics::AtomicStoreMax(IntPeak, Value);
				}
			}));
		}
	}

	double Expected = 1.0;
	for (int32 Step = 0; Step < NumThreads * UpdatesPerThread; ++Step)
	{
		Expected *= 0.5;
	}
	TestTrue(TEXT("No average update was lost"), Average.load() == Expected);
	TestEqual(TEXT("Peak (double) is the largest sample"), Peak.load(), static_cast<double>(NumThreads * UpdatesPerThread - 1));
	TestEqual(TEXT("Peak (int32) is the largest sample"), IntPeak.load(), NumThreads * UpdatesPerThread - 1);

	O3DMetrics::AtomicStoreMax(IntPeak, 3);
	TestEqual(TEXT("A smaller value never lowers the peak"), IntPeak.load(), NumThreads * UpdatesPerThread - 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DMetricsResetAndCsvTest, "Open3DBroadcast.Shared.Metrics.ResetClearsEverythingAndCsvListsTransports", O3DB_TEST_FLAGS)
bool FO3DMetricsResetAndCsvTest::RunTest(const FString& Parameters)
{
	// SHR-25: Reset left the per-operation timings and subject counts behind, and the CSV had no
	// transport rows.
	FO3DPerformanceMetrics& Metrics = FO3DPerformanceMetrics::Get();
	const FName TransportName(*FString::Printf(TEXT("MetricsCsvTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FO3DTransportMetricsRef Transport = Metrics.AcquireTransportMetrics(TransportName);
	Transport->RecordFrameSent(100);

	Metrics.RecordParseTimeMs(2.0);
	Metrics.RecordPoseExtractionTimeMs(2.0);
	Metrics.RecordLiveLinkPushTimeMs(2.0);
	Metrics.RecordTotalProcessingTimeMs(2.0);
	Metrics.SetReceiverActiveSubjectCount(3);
	Metrics.RecordSkeletonUpdate();
	Metrics.RecordFrameLatency(4.0);

	const FString Csv = Metrics.GetMetricsAsCSV();
	const FString Prefix = FString::Printf(TEXT("Transport.%s."), *TransportName.ToString());
	TestTrue(TEXT("The CSV lists the transport's frames"), Csv.Contains(Prefix + TEXT("FramesSent,1")));
	TestTrue(TEXT("And its bytes"), Csv.Contains(Prefix + TEXT("BytesSent,100")));
	TestTrue(TEXT("The CSV has the per-operation timings"), Csv.Contains(TEXT("AvgParseTimeMs,")));
	TestTrue(TEXT("And the skeleton updates"), Csv.Contains(TEXT("ReceiverSkeletonUpdates,")));
	TestTrue(TEXT("Header row"), Csv.StartsWith(TEXT("Metric,Value")));

	Metrics.Reset();
	const FO3DPerformanceMetrics::FReceiverMetrics& Receiver = Metrics.GetReceiverMetrics();
	TestEqual(TEXT("Parse time reset"), Receiver.AvgParseTimeMs.load(), 0.0);
	TestEqual(TEXT("Pose extraction time reset"), Receiver.AvgPoseExtractionTimeMs.load(), 0.0);
	TestEqual(TEXT("LiveLink push time reset"), Receiver.AvgLiveLinkPushTimeMs.load(), 0.0);
	TestEqual(TEXT("Total processing time reset"), Receiver.AvgTotalProcessingTimeMs.load(), 0.0);
	TestEqual(TEXT("Active subjects reset"), Receiver.ActiveSubjectCount.load(), 0);
	TestEqual(TEXT("Skeleton updates reset"), Receiver.SkeletonUpdates.load(), 0ull);
	TestEqual(TEXT("Receive-to-apply latency reset"), Receiver.AvgReceiveToApplyLatencyMs.load(), 0.0);
	TestEqual(TEXT("Transport counters reset"), Transport->FramesSent.load(), 0ull);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
