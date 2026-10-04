// Copyright Lifelike & Believable. All Rights Reserved.

// RCV-8, ADR 0013 PR 3: FO3DSceneTimeMapper, the LiveLink SceneTime of each pushed frame, with
// its clocks passed in. The sender's timecode is used exactly; a subject keeps the rate of its
// first timecode and later ones are converted to it; without sender timecode the frame continues
// the sender's timeline, or (option A') takes WorldTime on the engine's timecode, strictly
// increasing even when the engine timecode steps in whole frames; no engine timecode, no
// SceneTime. Also through the LiveLink publisher, whose frames carry what the mapper returns.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Testing/O3DReceiverTesting.h"

namespace O3DSceneTimeMapperTests
{
	const FName Hero(TEXT("Hero"));

	/** Engine timecode at Seconds, whole frames only (as USystemTimeTimecodeProvider by default). */
	FQualifiedFrameTime WholeFrameEngineTime(double Seconds, FFrameRate Rate)
	{
		return FQualifiedFrameTime(FFrameTime(Rate.AsFrameTime(Seconds).GetFrame()), Rate);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSceneTimeMapperSenderTest, "Open3DBroadcast.Receiver.SceneTime.SenderTimecodeAndRate", O3DB_TEST_FLAGS)
bool FO3DSceneTimeMapperSenderTest::RunTest(const FString& Parameters)
{
	using namespace O3DSceneTimeMapperTests;
	FO3DSceneTimeMapperProbe Mapper;
	const TOptional<FQualifiedFrameTime> NoEngine;

	// Exactly the sender's timecode.
	const TOptional<FQualifiedFrameTime> First = Mapper.MapSender(Hero, 1000, 0.25f, 24, 1, 10.0, 10.0, NoEngine);
	if (!TestTrue(TEXT("Sender timecode gives a SceneTime"), First.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("Frame"), First->Time.GetFrame().Value, 1000);
	TestEqual(TEXT("Sub-frame"), First->Time.GetSubFrame(), 0.25f);
	TestTrue(TEXT("Rate"), First->Rate == FFrameRate(24, 1));

	// A frame at another rate keeps the subject's rate, converted (a rate change flushes LiveLink's buffer).
	const TOptional<FQualifiedFrameTime> Converted = Mapper.MapSender(Hero, 1260, 0.0f, 30, 1, 11.0, 11.0, NoEngine); // 42 s
	TestTrue(TEXT("Still the subject's first rate"), Converted.IsSet() && Converted->Rate == FFrameRate(24, 1));
	TestTrue(TEXT("Same instant at 24 fps"), Converted.IsSet() && FMath::IsNearlyEqual(Converted->AsSeconds(), 42.0, 1.0e-6));

	// Without sender timecode the sender's timeline continues: last sender time plus elapsed WorldTime.
	const TOptional<FQualifiedFrameTime> Continued = Mapper.MapWithout(Hero, 11.5, 11.5, NoEngine);
	TestTrue(TEXT("Continues the sender's timeline"), Continued.IsSet() && FMath::IsNearlyEqual(Continued->AsSeconds(), 42.5, 1.0e-6));

	// A forgotten subject starts again: its rate comes from its next timecode.
	Mapper.ForgetSubject(Hero);
	const TOptional<FQualifiedFrameTime> Again = Mapper.MapSender(Hero, 10, 0.0f, 30, 1, 12.0, 12.0, NoEngine);
	TestTrue(TEXT("New rate after ForgetSubject"), Again.IsSet() && Again->Rate == FFrameRate(30, 1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSceneTimeMapperFallbackTest, "Open3DBroadcast.Receiver.SceneTime.FallbackOnEngineTimecode", O3DB_TEST_FLAGS)
bool FO3DSceneTimeMapperFallbackTest::RunTest(const FString& Parameters)
{
	using namespace O3DSceneTimeMapperTests;
	FO3DSceneTimeMapperProbe Mapper;

	// No sender timecode and no engine timecode: nothing.
	TestFalse(TEXT("No timecode anywhere: unset"), Mapper.MapWithout(Hero, 5.0, 5.0, TOptional<FQualifiedFrameTime>()).IsSet());

	// Engine timecode in whole frames at 24 fps, 3600 s ahead of the platform clock; a 120 Hz sender
	// whose frames are presented 20 ms after now. Every result increases, at the engine rate.
	const FFrameRate Rate(24, 1);
	constexpr double EngineAhead = 3600.0;
	double Previous = -1.0;
	bool bIncreasing = true;
	bool bRate = true;
	bool bNearWorldTime = true;
	for (int32 Index = 0; Index < 240; ++Index)
	{
		const double Now = 100.0 + Index / 120.0;
		const double WorldTime = Now + 0.020;
		const TOptional<FQualifiedFrameTime> Engine = WholeFrameEngineTime(Now + EngineAhead, Rate);
		const TOptional<FQualifiedFrameTime> Mapped = Mapper.MapWithout(Hero, WorldTime, Now, Engine);
		if (!Mapped.IsSet())
		{
			AddError(FString::Printf(TEXT("Frame %d: no SceneTime with an engine timecode"), Index));
			return false;
		}
		const double Seconds = Mapped->AsSeconds();
		bIncreasing &= Seconds > Previous;
		bRate &= Mapped->Rate == Rate;
		// Within a frame of WorldTime on the engine's timecode (the offset is taken once).
		bNearWorldTime &= FMath::Abs(Seconds - (WorldTime + EngineAhead)) <= Rate.AsInterval() + 1.0e-6;
		Previous = Seconds;
	}
	TestTrue(TEXT("Strictly increasing with a whole-frame engine timecode"), bIncreasing);
	TestTrue(TEXT("At the engine's rate"), bRate);
	TestTrue(TEXT("WorldTime on the engine's timecode"), bNearWorldTime);

	// The engine timecode jumps (a new provider, a jam sync): the offset follows.
	const double Now = 200.0;
	const TOptional<FQualifiedFrameTime> AfterJump = Mapper.MapWithout(Hero, Now, Now, FQualifiedFrameTime(Rate.AsFrameTime(Now + 7200.0), Rate));
	TestTrue(TEXT("Offset taken again after a jump"), AfterJump.IsSet() && FMath::Abs(AfterJump->AsSeconds() - (Now + 7200.0)) <= Rate.AsInterval());

	// Never negative: LiveLink rejects a frame before frame 0.
	Mapper.Reset();
	TestFalse(TEXT("A negative derived time is unset"), Mapper.MapWithout(Hero, 1.0, 10.0, FQualifiedFrameTime(Rate.AsFrameTime(2.0), Rate)).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLiveLinkPublisherSceneTimeTest, "Open3DBroadcast.Receiver.SceneTime.PublisherFramesCarryIt", O3DB_TEST_FLAGS)
bool FO3DLiveLinkPublisherSceneTimeTest::RunTest(const FString& Parameters)
{
	using namespace O3DSceneTimeMapperTests;
	const TOptional<FQualifiedFrameTime> Saved = FApp::GetCurrentFrameTime();
	ON_SCOPE_EXIT
	{
		if (Saved.IsSet())
		{
			FApp::SetCurrentFrameTime(Saved.GetValue());
		}
		else
		{
			FApp::InvalidateCurrentFrameTime();
		}
	};
	FApp::InvalidateCurrentFrameTime();

	FO3DLiveLinkPublisherProbe Probe;
	Probe.PublishStatic(Hero, { FName(TEXT("root")) }, { -1 }, {});
	Probe.PublishFrameWithSenderTime(Hero, 1, 10.0, 240, 0.0f, 24, 1);
	Probe.PublishSyntheticFrame(Hero, 1, 10.25);
	Probe.PublishFrame(Hero, 1, 10.5); // no sender timecode on this one
	if (!TestEqual(TEXT("Three frames"), Probe.Frames.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Real frame: the sender's timecode"), Probe.Frames[0].SceneTime.IsSet() && Probe.Frames[0].SceneTime->Time.GetFrame().Value == 240);
	TestTrue(TEXT("Synthetic frame: on the sender's timeline"), Probe.Frames[1].SceneTime.IsSet()
		&& FMath::IsNearlyEqual(Probe.Frames[1].SceneTime->AsSeconds(), 10.25, 1.0e-6));
	TestTrue(TEXT("Frame without one: continues the timeline"), Probe.Frames[2].SceneTime.IsSet()
		&& FMath::IsNearlyEqual(Probe.Frames[2].SceneTime->AsSeconds(), 10.5, 1.0e-6));

	// A subject that never had sender timecode, with no engine timecode: LiveLink's default.
	const FName Villain(TEXT("Villain"));
	Probe.PublishStatic(Villain, { FName(TEXT("root")) }, { -1 }, {});
	Probe.PublishFrame(Villain, 1, 11.0);
	TestFalse(TEXT("No timecode anywhere: SceneTime left unset"), Probe.Frames.Last().SceneTime.IsSet());

	// Reset forgets the timeline.
	Probe.Reset();
	Probe.PublishStatic(Hero, { FName(TEXT("root")) }, { -1 }, {});
	Probe.PublishFrame(Hero, 1, 12.0);
	TestFalse(TEXT("After Reset the old timeline is gone"), Probe.Frames.Last().SceneTime.IsSet());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
