// Copyright Lifelike & Believable. All Rights Reserved.
//
// FO3DTransportWorker and FO3DReconnectPolicy (ADR 0007 item 7, WP-A1 step 4; TRB-4, TRF-6, TRF-20).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DTransportWorker.h"

#include <atomic>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportWorkerRunsAndStopsTest, "Open3DBroadcast.Shared.TransportWorker.RunsAndStops", O3DB_TEST_FLAGS)
bool FO3DTransportWorkerRunsAndStopsTest::RunTest(const FString& Parameters)
{
	FO3DTransportWorker Worker;
	TestFalse(TEXT("A worker without a body does not start"), Worker.Start(TEXT("O3D_TestWorker"), FO3DTransportWorker::FBody()));

	const uint32 GameThreadId = FPlatformTLS::GetCurrentThreadId();
	std::atomic<int32> Runs{ 0 };
	std::atomic<uint32> BodyThread{ 0 };
	TestTrue(TEXT("Start"), Worker.Start(TEXT("O3D_TestWorker"), [&Runs, &BodyThread]()
	{
		BodyThread.store(FPlatformTLS::GetCurrentThreadId());
		Runs.fetch_add(1);
		return 1u;
	}));
	TestTrue(TEXT("Running"), Worker.IsRunning());
	TestFalse(TEXT("A second Start is refused while running"), Worker.Start(TEXT("O3D_TestWorker"), []() { return 1u; }));
	TestTrue(TEXT("The body runs repeatedly"), O3DTests::PollUntil(5.0, [&Runs]() { return Runs.load() >= 3; }));
	TestTrue(TEXT("The body runs on the worker thread"), BodyThread.load() != 0 && BodyThread.load() != GameThreadId);

	Worker.Stop();
	TestFalse(TEXT("Stopped"), Worker.IsRunning());
	TestTrue(TEXT("Stop requested"), Worker.IsStopRequested());
	const int32 RunsAtStop = Runs.load();
	TestEqual(TEXT("Iterations counted"), static_cast<int32>(Worker.GetIterations()), RunsAtStop);
	Worker.Stop(); // idempotent
	TestEqual(TEXT("Nothing runs after Stop"), Runs.load(), RunsAtStop);

	// Restartable with a new body.
	std::atomic<int32> SecondRuns{ 0 };
	TestTrue(TEXT("Restart"), Worker.Start(TEXT("O3D_TestWorker"), [&SecondRuns]() { SecondRuns.fetch_add(1); return 1u; }));
	TestFalse(TEXT("Stop request cleared by Start"), Worker.IsStopRequested());
	TestTrue(TEXT("The new body runs"), O3DTests::PollUntil(5.0, [&SecondRuns]() { return SecondRuns.load() > 0; }));
	// Stop before the counters the body uses go out of scope.
	Worker.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportWorkerWakeTest, "Open3DBroadcast.Shared.TransportWorker.WakesOnEnqueueAndStop", O3DB_TEST_FLAGS)
bool FO3DTransportWorkerWakeTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue = MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>();
	std::atomic<int32> Drained{ 0 };
	FO3DTransportWorker Worker;
	// The body waits 60 s between runs, so only a wake can make the next item come out quickly.
	TestTrue(TEXT("Start"), Worker.Start(TEXT("O3D_TestWorker"), [Queue, &Drained]()
	{
		FO3DSendItem Item;
		while (Queue->Dequeue(Item))
		{
			Drained.fetch_add(1);
		}
		return 60000u;
	}, Queue));
	TestTrue(TEXT("First run"), O3DTests::PollUntil(5.0, [&Worker]() { return Worker.GetIterations() >= 1; }));

	TArray<uint8> Bytes;
	Bytes.Add(1);
	TestTrue(TEXT("Enqueue"), Queue->Enqueue(FO3DSendItem::MakeMocap(MoveTemp(Bytes), TEXT("S"), 0.0)) == EO3DSendResult::Queued);
	TestTrue(TEXT("An Enqueue wakes the worker"), O3DTests::PollUntil(5.0, [&Drained]() { return Drained.load() == 1; }));

	const int64 Before = Worker.GetIterations();
	Worker.Wake();
	TestTrue(TEXT("Wake runs the body again"), O3DTests::PollUntil(5.0, [&Worker, Before]() { return Worker.GetIterations() > Before; }));

	const double StopStart = FPlatformTime::Seconds();
	Worker.Stop();
	TestTrue(TEXT("Stop does not wait for the 60 s idle wait"), FPlatformTime::Seconds() - StopStart < 5.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReconnectPolicyBackoffTest, "Open3DBroadcast.Shared.ReconnectPolicy.ExponentialBackoff", O3DB_TEST_FLAGS)
bool FO3DReconnectPolicyBackoffTest::RunTest(const FString& Parameters)
{
	FO3DReconnectPolicySettings Settings;
	Settings.InitialDelaySeconds = 0.5;
	Settings.Multiplier = 2.0;
	Settings.MaxDelaySeconds = 4.0;
	Settings.JitterFraction = 0.0;
	FO3DReconnectPolicy Policy(Settings, 1);

	TestTrue(TEXT("Due before any failure"), Policy.IsDue(0.0));
	const double Expected[] = { 0.5, 1.0, 2.0, 4.0, 4.0, 4.0 };
	double Now = 100.0;
	for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(Expected)); ++Index)
	{
		const double Delay = Policy.OnFailure(Now);
		TestEqual(*FString::Printf(TEXT("Delay after failure %d"), Index + 1), Delay, Expected[Index]);
		TestFalse(TEXT("Not due before the delay"), Policy.IsDue(Now + Delay * 0.5));
		TestTrue(TEXT("Due after the delay"), Policy.IsDue(Now + Delay));
		Now += Delay;
	}
	TestEqual(TEXT("Failures counted"), Policy.GetFailedAttempts(), 6);
	TestFalse(TEXT("Unlimited attempts never exhaust"), Policy.IsExhausted());
	TestEqual(TEXT("Base delay of a long outage is capped"), Policy.GetBaseDelaySeconds(10000), 4.0);

	Policy.OnSuccess();
	TestEqual(TEXT("Success resets the count"), Policy.GetFailedAttempts(), 0);
	TestTrue(TEXT("Due at once after success"), Policy.IsDue(Now));
	TestEqual(TEXT("The next failure starts over"), Policy.OnFailure(Now), 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReconnectPolicyJitterTest, "Open3DBroadcast.Shared.ReconnectPolicy.JitterAndLimits", O3DB_TEST_FLAGS)
bool FO3DReconnectPolicyJitterTest::RunTest(const FString& Parameters)
{
	FO3DReconnectPolicySettings Settings;
	Settings.InitialDelaySeconds = 1.0;
	Settings.Multiplier = 2.0;
	Settings.MaxDelaySeconds = 8.0;
	Settings.JitterFraction = 0.25;
	FO3DReconnectPolicy A(Settings, 42);
	FO3DReconnectPolicy B(Settings, 42);

	bool bWithinBounds = true;
	bool bSameSequence = true;
	bool bSawJitter = false;
	for (int32 Failure = 1; Failure <= 20; ++Failure)
	{
		const double Base = A.GetBaseDelaySeconds(Failure);
		const double DelayA = A.OnFailure(0.0);
		const double DelayB = B.OnFailure(0.0);
		bWithinBounds &= DelayA >= Base * 0.75 - 1e-9 && DelayA <= FMath::Min(Base * 1.25, 8.0) + 1e-9;
		bSameSequence &= DelayA == DelayB;
		bSawJitter |= !FMath::IsNearlyEqual(DelayA, Base, 1e-6);
	}
	TestTrue(TEXT("Every delay is within the jitter band and the cap"), bWithinBounds);
	TestTrue(TEXT("The same seed gives the same delays"), bSameSequence);
	TestTrue(TEXT("Jitter changes the delays"), bSawJitter);

	FO3DReconnectPolicySettings Limited;
	Limited.JitterFraction = 0.0;
	Limited.MaxAttempts = 3;
	FO3DReconnectPolicy Policy(Limited, 7);
	TestTrue(TEXT("1st failure schedules a retry"), Policy.OnFailure(0.0) >= 0.0);
	TestTrue(TEXT("2nd failure schedules a retry"), Policy.OnFailure(10.0) >= 0.0);
	TestTrue(TEXT("3rd failure gives up"), Policy.OnFailure(20.0) < 0.0);
	TestTrue(TEXT("Exhausted"), Policy.IsExhausted());
	TestFalse(TEXT("Never due once exhausted"), Policy.IsDue(1.0e9));
	Policy.Reset();
	TestTrue(TEXT("Reset makes it due again"), Policy.IsDue(0.0) && !Policy.IsExhausted());

	FO3DReconnectPolicySettings Odd;
	Odd.InitialDelaySeconds = -1.0;
	Odd.Multiplier = 0.5;
	Odd.JitterFraction = 3.0;
	FO3DReconnectPolicy Clamped(Odd, 3);
	TestTrue(TEXT("Settings are clamped"), Clamped.GetSettings().InitialDelaySeconds == 0.0 && Clamped.GetSettings().Multiplier == 1.0 && Clamped.GetSettings().JitterFraction == 1.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
