// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// FO3DReceiverStreamScheduler and FO3DReceiverConcealment (WP-A3 step 3, RCV-29), reached through
// their exported probes. The scheduler orders each sender's packets (legacy timestamp ordering, or
// the reorder gate for sequenced packets) and releases them to an apply callback; concealment
// synthesizes frames for a subject whose gated real frames starve.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DReceiverSourceSettings.h"
#include "Testing/O3DReceiverTesting.h"
#include "UObject/Package.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

namespace O3DReceiverStreamSchedulerTests
{
	/** One subject, one bone, serialized by the core with the given time and sequencing fields. */
	TArray<uint8> MakeSchedulerPacket(const std::string& Subject, double Time, uint64 TxSeq, uint64 WallclockUs = 0, uint32 Epoch = 0)
	{
		O3DS::SubjectList List;
		O3DS::Subject* SubjectObject = List.addSubject(Subject);
		O3DS::Transform* Root = SubjectObject->addTransform("root", -1);
		Root->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
		std::vector<char> Buffer;
		List.Serialize(Buffer, Time, TxSeq, WallclockUs, Epoch);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Buffer.data()), static_cast<int32>(Buffer.size()));
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverStreamSchedulerLegacyTest, "Open3DBroadcast.Receiver.StreamScheduler.LegacyOrderingDropsStaleFrames", O3DB_TEST_FLAGS)
bool FO3DReceiverStreamSchedulerLegacyTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverStreamSchedulerTests;

	FO3DReceiverStreamSchedulerProbe Probe;
	TestTrue(TEXT("t=1.0 pushed"), Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.0, 0), 5.0, 100.0));
	TestTrue(TEXT("t=1.0 again pushed"), Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.0, 0), 5.1, 100.01));
	TestTrue(TEXT("t=0.9 pushed"), Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 0.9, 0), 5.2, 100.02));
	TestTrue(TEXT("t=1.1 pushed"), Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.1, 0), 5.3, 100.03));

	TestEqual(TEXT("The duplicate and the older frame are dropped"), Probe.Released.Num(), 2);
	TestTrue(TEXT("Legacy releases are not gated"), Probe.Released.Num() == 2 && !Probe.Released[0].bGated && !Probe.Released[1].bGated);
	TestTrue(TEXT("They carry the transport's timestamp"), Probe.Released.Num() == 2 && Probe.Released[0].LegacyTimestampSeconds == 5.0 && Probe.Released[1].LegacyTimestampSeconds == 5.3);
	TestTrue(TEXT("And the subject label"), Probe.Released.Num() == 2 && Probe.Released[0].Label == TEXT("Hero"));
	TestFalse(TEXT("A malformed packet does not verify"), Probe.Push(TEXT("Hero"), TArray<uint8>({ 1, 2, 3 }), 6.0, 100.04));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverStreamSchedulerGateTest, "Open3DBroadcast.Receiver.StreamScheduler.GateReordersPerSender", O3DB_TEST_FLAGS)
bool FO3DReceiverStreamSchedulerGateTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverStreamSchedulerTests;

	FO3DReceiverStreamSchedulerProbe Probe;
	Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.0, 1, 1000, 7), 0.0, 10.0);
	Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.2, 3, 1200, 7), 0.0, 10.001);
	TestEqual(TEXT("Seq 3 waits for the gap"), Probe.Released.Num(), 1);
	Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.1, 2, 1100, 7), 0.0, 10.002);
	TestEqual(TEXT("Seq 2 fills it and both go"), Probe.Released.Num(), 3);
	TestTrue(TEXT("In sequence order"), Probe.Released.Num() == 3 && Probe.Released[1].Seq == 2 && Probe.Released[2].Seq == 3);
	TestTrue(TEXT("Gated"), Probe.Released.Num() == 3 && Probe.Released[2].bGated);

	Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.1, 2, 1100, 7), 0.0, 10.003);
	TestEqual(TEXT("A duplicate is dropped"), Probe.Released.Num(), 3);

	// Another sender on the same channel has its own stream and sequence space (RCV-5).
	Probe.Push(TEXT("Villain"), MakeSchedulerPacket("Villain", 5.0, 1, 5000, 9), 0.0, 10.004);
	TestEqual(TEXT("Another sender's seq 1 is not a duplicate"), Probe.Released.Num(), 4);
	TestEqual(TEXT("Two streams"), Probe.GetNumStreams(), 2);

	// A gap that never fills is given up after the gate's wait, on Flush.
	Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 1.5, 5, 1500, 7), 0.0, 10.010);
	TestEqual(TEXT("Seq 5 waits for seq 4"), Probe.Released.Num(), 4);
	Probe.Flush(10.011);
	TestEqual(TEXT("Not yet"), Probe.Released.Num(), 4);
	Probe.Flush(11.0);
	TestTrue(TEXT("Released once the wait timed out"), Probe.Released.Num() == 5 && Probe.Released[4].Seq == 5);

	// Idle streams are pruned; Reset drops everything.
	Probe.PruneIdle(100.0, 5.0);
	TestEqual(TEXT("Idle streams pruned"), Probe.GetNumStreams(), 0);
	Probe.Push(TEXT("Hero"), MakeSchedulerPacket("Hero", 2.0, 1, 2000, 8), 0.0, 101.0);
	Probe.Reset();
	TestEqual(TEXT("Reset drops every stream"), Probe.GetNumStreams(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverConcealmentTest, "Open3DBroadcast.Receiver.Concealment.SynthesizesOnlyWhenStarved", O3DB_TEST_FLAGS)
bool FO3DReceiverConcealmentTest::RunTest(const FString& Parameters)
{
	const FName Hero(TEXT("Hero"));
	FO3DReceiverConcealmentProbe Probe;

	// Real frames at 60 Hz, the root moving along X.
	double LastTime = 0.0;
	for (int32 Index = 0; Index < 10; ++Index)
	{
		LastTime = Index / 60.0;
		const TArray<FTransform> Pose = { FTransform(FVector(10.0 * Index, 0.0, 0.0)) };
		Probe.ObserveRealFrame(nullptr, Hero, LastTime, Pose, Index == 0);
	}
	TestEqual(TEXT("One engine for the subject"), Probe.GetNumEngines(), 1);

	Probe.Tick(nullptr, true, LastTime + 0.5);
	TestEqual(TEXT("Nothing before the gated path noted a clock offset"), Probe.Synthetic.Num(), 0);

	Probe.NoteClockOffset(1234);
	TestTrue(TEXT("Clock offset noted"), Probe.HasClockOffsetEstimate());

	Probe.Tick(nullptr, false, LastTime + 0.1);
	TestEqual(TEXT("Nothing while frames cannot be published"), Probe.Synthetic.Num(), 0);

	Probe.Tick(nullptr, true, LastTime + 0.005);
	TestEqual(TEXT("Nothing while real frames are on time"), Probe.Synthetic.Num(), 0);

	Probe.Tick(nullptr, true, LastTime + 0.1);
	TestTrue(TEXT("A synthesized frame once real frames starve"), Probe.Synthetic.Num() >= 1);
	TestTrue(TEXT("For the subject, with its bones"), Probe.Synthetic.Num() >= 1 && Probe.Synthetic[0].Subject == Hero && Probe.Synthetic[0].Transforms.Num() == 1);

	UO3DReceiverSourceSettings* Disabled = NewObject<UO3DReceiverSourceSettings>(GetTransientPackage());
	Disabled->bEnableConcealment = false;
	const int32 Before = Probe.Synthetic.Num();
	Probe.Tick(Disabled, true, LastTime + 0.12);
	TestEqual(TEXT("Nothing while concealment is disabled"), Probe.Synthetic.Num(), Before);
	Probe.ObserveRealFrame(Disabled, FName(TEXT("Other")), LastTime, { FTransform::Identity }, true);
	TestEqual(TEXT("No engine created while disabled"), Probe.GetNumEngines(), 1);

	Probe.ForgetSubject(Hero);
	TestEqual(TEXT("ForgetSubject drops the engine"), Probe.GetNumEngines(), 0);
	Probe.Reset();
	TestFalse(TEXT("Reset forgets the clock offset"), Probe.HasClockOffsetEstimate());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
