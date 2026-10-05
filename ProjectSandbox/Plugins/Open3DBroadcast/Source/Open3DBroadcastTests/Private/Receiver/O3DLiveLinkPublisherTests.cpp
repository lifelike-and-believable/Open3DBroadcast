// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// FO3DLiveLinkPublisher (WP-A3, RCV-29): creates a LiveLink subject once per session (RCV-7),
// re-pushes static data only when bone or curve names change, pushes real and synthesized frames,
// and removes subjects that stopped sending. Reached through the exported FO3DLiveLinkPublisherProbe,
// whose test hooks record the pushes instead of a LiveLink client.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Testing/O3DReceiverTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLiveLinkPublisherStaticTest, "Open3DBroadcast.Receiver.LiveLinkPublisher.StaticDataOncePerSessionAndOnChange", O3DB_TEST_FLAGS)
bool FO3DLiveLinkPublisherStaticTest::RunTest(const FString& Parameters)
{
	const FName Hero(TEXT("Hero"));
	const TArray<FName> Bones = { FName(TEXT("root")), FName(TEXT("arm")) };
	const TArray<int32> Parents = { -1, 0 };
	const TArray<FName> Curves = { FName(TEXT("brow")) };

	FO3DLiveLinkPublisherProbe Probe;
	TestTrue(TEXT("Hooks count as a client"), Probe.CanPublish());

	TestTrue(TEXT("A new subject is a topology change"), Probe.PublishStatic(Hero, Bones, Parents, Curves));
	TestEqual(TEXT("One static push"), Probe.Statics.Num(), 1);
	TestTrue(TEXT("It is the first push this session (creates the subject)"), Probe.Statics.Num() == 1 && Probe.Statics[0].bFirstPushThisSession);

	TestFalse(TEXT("The same names are no change"), Probe.PublishStatic(Hero, Bones, Parents, Curves));
	TestEqual(TEXT("No second push"), Probe.Statics.Num(), 1);

	const TArray<FName> MoreCurves = { FName(TEXT("brow")), FName(TEXT("jaw")) };
	TestTrue(TEXT("New curve names are a change"), Probe.PublishStatic(Hero, Bones, Parents, MoreCurves));
	TestEqual(TEXT("Static data pushed again"), Probe.Statics.Num(), 2);
	TestTrue(TEXT("Not as a first push (the subject's settings are kept)"), Probe.Statics.Num() == 2 && !Probe.Statics[1].bFirstPushThisSession);
	TestTrue(TEXT("With the new curve names"), Probe.Statics.Num() == 2 && Probe.Statics[1].CurveNames == MoreCurves);

	const TArray<int32> OtherParents = { -1, -1 };
	TestTrue(TEXT("New parents are a change"), Probe.PublishStatic(Hero, Bones, OtherParents, MoreCurves));
	TestEqual(TEXT("Pushed again"), Probe.Statics.Num(), 3);

	// A new session (the transport restarted) creates the subject again.
	Probe.Reset();
	TestTrue(TEXT("After Reset the subject is new"), Probe.PublishStatic(Hero, Bones, Parents, Curves));
	TestTrue(TEXT("And its push is a first push"), Probe.Statics.Num() == 4 && Probe.Statics[3].bFirstPushThisSession);

	FO3DLiveLinkPublisherProbe Unbound(false);
	TestFalse(TEXT("Without a client or hooks nothing can be published"), Unbound.CanPublish());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLiveLinkPublisherFramesTest, "Open3DBroadcast.Receiver.LiveLinkPublisher.FramesAndInactiveSubjects", O3DB_TEST_FLAGS)
bool FO3DLiveLinkPublisherFramesTest::RunTest(const FString& Parameters)
{
	const FName Hero(TEXT("Hero"));
	const FName Villain(TEXT("Villain"));
	const TArray<FName> Bones = { FName(TEXT("root")) };
	const TArray<int32> Parents = { -1 };
	const TArray<FName> NoCurves;

	FO3DLiveLinkPublisherProbe Probe;
	Probe.PublishStatic(Hero, Bones, Parents, NoCurves);
	Probe.PublishFrame(Hero, 1, 12.5);
	Probe.PublishStatic(Villain, Bones, Parents, NoCurves);
	Probe.PublishFrame(Villain, 1, 13.0);
	TestEqual(TEXT("Two frames pushed"), Probe.Frames.Num(), 2);
	TestTrue(TEXT("A real frame carries its presentation time"), Probe.Frames.Num() == 2 && Probe.Frames[0].WorldTime == 12.5);
	TestEqual(TEXT("Two active subjects"), Probe.GetActiveSubjectCount(), 2);

	// A synthesized frame: no static data, its own time, and it does not count as activity.
	Probe.PublishSyntheticFrame(Hero, 1, 14.0);
	TestEqual(TEXT("A synthesized frame is pushed"), Probe.Frames.Num(), 3);
	TestTrue(TEXT("With its own time"), Probe.Frames.Num() == 3 && Probe.Frames[2].WorldTime == 14.0);
	TestEqual(TEXT("Without static data"), Probe.Statics.Num(), 2);

	// Nothing is inactive yet.
	TestEqual(TEXT("Nothing removed within the threshold"), Probe.RemoveInactiveSubjects(FPlatformTime::Seconds(), 5.0).Num(), 0);

	// Ten seconds later both are inactive.
	const TArray<FName> Removed = Probe.RemoveInactiveSubjects(FPlatformTime::Seconds() + 10.0, 5.0);
	TestEqual(TEXT("Both removed"), Removed.Num(), 2);
	TestTrue(TEXT("Each reported"), Removed.Contains(Hero) && Removed.Contains(Villain));
	TestEqual(TEXT("No active subjects left"), Probe.GetActiveSubjectCount(), 0);

	// A removed subject is created again when it returns.
	TestTrue(TEXT("A returning subject is new"), Probe.PublishStatic(Hero, Bones, Parents, NoCurves));
	TestTrue(TEXT("And created again"), Probe.Statics.Num() == 3 && Probe.Statics[2].bFirstPushThisSession);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLiveLinkPublisherSlowPushTest, "Open3DBroadcast.Receiver.LiveLinkPublisher.ThrottlesSlowPushWarnings", O3DB_TEST_FLAGS)
bool FO3DLiveLinkPublisherSlowPushTest::RunTest(const FString& Parameters)
{
	// RCV-26: a sustained LiveLink stall logged a warning on every frame. Now at most one per 5 s,
	// with the count of the slow pushes not logged.
	AddExpectedError(TEXT("PushSubjectFrameData took"), EAutomationExpectedMessageFlags::Contains, 2);
	FO3DLiveLinkPublisherProbe Probe;
	const FName Subject(TEXT("Hero"));
	Probe.NoteSlowFramePush(Subject, 8.0, 100.0);
	TestEqual(TEXT("The first slow push is logged"), Probe.GetSlowPushesNotLogged(), 0);
	Probe.NoteSlowFramePush(Subject, 9.0, 101.0);
	Probe.NoteSlowFramePush(Subject, 9.0, 104.9);
	TestEqual(TEXT("Those within 5 s are counted, not logged"), Probe.GetSlowPushesNotLogged(), 2);
	Probe.NoteSlowFramePush(Subject, 7.0, 105.0);
	TestEqual(TEXT("After 5 s the next is logged, with the count"), Probe.GetSlowPushesNotLogged(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
