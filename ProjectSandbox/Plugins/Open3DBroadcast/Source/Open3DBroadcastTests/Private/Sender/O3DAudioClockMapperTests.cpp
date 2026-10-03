// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A2d (ADR 0008 item 7, SND-17): the submix tap and the microphone map their source clocks
// (AudioClock, StreamTimeSec) onto the sender clock with FO3DAudioClockMapper. These tests drive
// the mapper on a manual clock: a source clock, a sender clock and a delivery delay per buffer.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioClockMapper.h"

namespace O3DAudioClockMapperTests
{
	constexpr double BufferSec = 0.02;
	constexpr double SenderStartSec = 1000.0;

	/** A small deterministic generator for delivery jitter (no FMath::Rand state shared with other tests). */
	struct FJitter
	{
		uint32 State = 12345u;
		/** Uniform in [0, MaxSec). */
		double Next(double MaxSec)
		{
			State = State * 1664525u + 1013904223u;
			return MaxSec * static_cast<double>(State >> 8) / static_cast<double>(1u << 24);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioClockMapsOntoSenderClockTest, "Open3DBroadcast.Sender.AudioClock.MapsOntoSenderClock", O3DB_TEST_FLAGS)
bool FO3DAudioClockMapsOntoSenderClockTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioClockMapperTests;

	// The source clock starts at 0 (the mixer's AudioClock counts from the start of rendering);
	// every buffer arrives 5 ms after its source time, on a sender clock that reads 1000 s.
	FO3DAudioClockMapper Mapper;
	const double DelaySec = 0.005;
	double MaxError = 0.0;
	for (int32 Index = 0; Index < 500; ++Index)
	{
		const double SourceSec = Index * BufferSec;
		const double NowSec = SenderStartSec + SourceSec + DelaySec;
		const double Stamp = Mapper.Map(SourceSec, NowSec);
		MaxError = FMath::Max(MaxError, FMath::Abs(Stamp - NowSec));
	}
	TestTrue(FString::Printf(TEXT("Stamps are on the sender clock (max error %.9f s)"), MaxError), MaxError < 1e-6);
	TestEqual(TEXT("The offset is sender minus source"), Mapper.GetOffset(), SenderStartSec + DelaySec, 1e-9);
	TestEqual(TEXT("No discontinuity"), Mapper.GetResetCount(), 0);

	// Reset forgets the offset: the next buffer sets it again.
	Mapper.Reset();
	const double Restarted = Mapper.Map(0.0, 2000.0);
	TestEqual(TEXT("After Reset the first buffer sets the offset again"), Restarted, 2000.0, 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioClockDriftIsTrackedTest, "Open3DBroadcast.Sender.AudioClock.DriftDoesNotAccumulate", O3DB_TEST_FLAGS)
bool FO3DAudioClockDriftIsTrackedTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioClockMapperTests;

	// A device clock running 100 ppm slow drifts 60 ms behind the sender clock in 10 minutes.
	// The low-pass filter follows it; the lag of a first-order filter on a ramp is slope times
	// the time constant (1e-4 * 5 s = 0.5 ms).
	FO3DAudioClockMapper Mapper;
	const double Rate = 1.0 - 1e-4;
	const int32 NumBuffers = 30000; // 600 s
	double Stamp = 0.0;
	double NowSec = 0.0;
	for (int32 Index = 0; Index < NumBuffers; ++Index)
	{
		NowSec = SenderStartSec + Index * BufferSec;
		const double SourceSec = Index * BufferSec * Rate;
		Stamp = Mapper.Map(SourceSec, NowSec);
	}
	const double Error = FMath::Abs(Stamp - NowSec);
	TestTrue(FString::Printf(TEXT("After 60 ms of drift the stamp is within 2 ms of the sender clock (%.6f s)"), Error), Error < 0.002);
	TestEqual(TEXT("Drift is not a discontinuity"), Mapper.GetResetCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioClockJitterTest, "Open3DBroadcast.Sender.AudioClock.JitterBarelyMovesStamps", O3DB_TEST_FLAGS)
bool FO3DAudioClockJitterTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioClockMapperTests;

	// Delivery delays vary from 0 to 10 ms per buffer. The stamps still advance by the source
	// step: one buffer's deviation moves the offset by about 0.4 % of it.
	FO3DAudioClockMapper Mapper;
	FJitter Jitter;
	double PreviousStamp = 0.0;
	double WorstStepError = 0.0;
	for (int32 Index = 0; Index < 3000; ++Index)
	{
		const double SourceSec = Index * BufferSec;
		const double NowSec = SenderStartSec + SourceSec + Jitter.Next(0.010);
		const double Stamp = Mapper.Map(SourceSec, NowSec);
		if (Index > 0)
		{
			WorstStepError = FMath::Max(WorstStepError, FMath::Abs((Stamp - PreviousStamp) - BufferSec));
		}
		PreviousStamp = Stamp;
	}
	TestTrue(FString::Printf(TEXT("Each stamp advances by the source step within 0.1 ms (worst %.7f s)"), WorstStepError), WorstStepError < 0.0001);
	TestEqual(TEXT("Jitter is not a discontinuity"), Mapper.GetResetCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioClockDiscontinuityTest, "Open3DBroadcast.Sender.AudioClock.DiscontinuitiesReset", O3DB_TEST_FLAGS)
bool FO3DAudioClockDiscontinuityTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioClockMapperTests;

	FO3DAudioClockMapper Mapper;
	const double DelaySec = 0.004;
	double NowSec = SenderStartSec;
	double SourceSec = 0.0;
	double PreviousStamp = -1.0;
	int32 Regressions = 0;
	auto Deliver = [&](double InSourceSec, double InNowSec)
	{
		const double Stamp = Mapper.Map(InSourceSec, InNowSec);
		if (Stamp < PreviousStamp)
		{
			++Regressions;
		}
		PreviousStamp = Stamp;
		return Stamp;
	};
	auto Steady = [&](int32 NumBuffers)
	{
		for (int32 Index = 0; Index < NumBuffers; ++Index)
		{
			SourceSec += BufferSec;
			NowSec += BufferSec;
			Deliver(SourceSec, NowSec + DelaySec);
		}
	};

	Steady(100);

	// 1. A hitch: one callback arrives 200 ms late, then the queued buffers arrive 1 ms apart.
	// A delay only makes the measured offset larger and recovers on its own: no reset.
	SourceSec += BufferSec;
	NowSec += BufferSec + 0.2;
	Deliver(SourceSec, NowSec + DelaySec);
	for (int32 Index = 0; Index < 10; ++Index)
	{
		SourceSec += BufferSec;
		NowSec += 0.001;
		Deliver(SourceSec, NowSec + DelaySec);
	}
	// The sender clock is now ahead of where the source puts it by 0.2 - 10 * 0.019 = 10 ms,
	// which steady delivery absorbs.
	Steady(100);
	TestEqual(TEXT("A 200 ms hitch does not reset the offset"), Mapper.GetResetCount(), 0);

	// 2. The source clock goes backwards (the device stream restarted): reset at once.
	SourceSec = 0.0;
	NowSec += BufferSec;
	const double AfterRestart = Deliver(SourceSec, NowSec + DelaySec);
	TestEqual(TEXT("A source clock going backwards resets the offset"), Mapper.GetResetCount(), 1);
	TestEqual(TEXT("And the stamp is the sender clock again"), AfterRestart, NowSec + DelaySec, 1e-9);
	Steady(50);

	// 3. The source clock jumps 1 s ahead: the stamps would run ahead of the sender clock, which
	// a delay cannot cause, so reset at once.
	SourceSec += 1.0;
	NowSec += BufferSec;
	const double AfterLeap = Deliver(SourceSec, NowSec + DelaySec);
	TestEqual(TEXT("A source clock jumping ahead resets the offset"), Mapper.GetResetCount(), 2);
	TestEqual(TEXT("And the stamp is the sender clock again"), AfterLeap, NowSec + DelaySec, 1e-9);
	Steady(50);

	// 4. The source clock loses 300 ms for good (it stalled while the sender clock ran on): the
	// measured offset stays 300 ms high. Reset once that has lasted PersistSec.
	NowSec += 0.3;
	const int32 ResetsBefore = Mapper.GetResetCount();
	int32 BuffersUntilReset = -1;
	for (int32 Index = 0; Index < 100 && BuffersUntilReset < 0; ++Index)
	{
		SourceSec += BufferSec;
		NowSec += BufferSec;
		Deliver(SourceSec, NowSec + DelaySec);
		if (Mapper.GetResetCount() > ResetsBefore)
		{
			BuffersUntilReset = Index + 1;
		}
	}
	// The first high buffer starts the timer; the reset comes PersistSec of sender time later
	// (one buffer more when the accumulated sender clock lands a rounding error short of it).
	const int32 ExpectedBuffers = FMath::RoundToInt(FO3DAudioClockMapper::PersistSec / BufferSec) + 1;
	TestTrue(FString::Printf(TEXT("A lasting offset change resets once it has lasted PersistSec (after %d buffers, expected %d)"), BuffersUntilReset, ExpectedBuffers),
		BuffersUntilReset == ExpectedBuffers || BuffersUntilReset == ExpectedBuffers + 1);
	Steady(10);
	TestEqual(TEXT("Stamps follow the sender clock after the reset"), PreviousStamp, NowSec + DelaySec, 1e-6);

	TestEqual(TEXT("Stamps never went backwards"), Regressions, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
