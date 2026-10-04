// Copyright Lifelike & Believable. All Rights Reserved.

// ADR 0005 (ix) through the real receiver (CORE-5, CORE-6): residual frames from the UE serializer
// are delivered to FO3DReceiverSource's serialized-frame consumer with one frame withheld. The
// reorder gate gives up on the gap once its window fills; the frames it then releases are parsed
// with their sequence context, so their residual updates are dropped and the subject is not
// pushed to LiveLink until the next full Subject, after which pushes resume.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "O3DPerformanceMetrics.h"
#include "O3DReceiverSource.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DSerializedFrameConsumer.h"

namespace O3DReceiverResyncTests
{
	FO3DSPoseFrame MakeResidualFrame(const FString& Subject, const TSharedPtr<const FO3DSSkeletonDescriptor>& Descriptor, double Time)
	{
		FO3DSPoseFrame Frame;
		Frame.Subject = Subject;
		Frame.Descriptor = Descriptor;
		Frame.CaptureTimeSec = Time;
		Frame.Encoding.Mode = EO3DSenderEncodingMode::Residual;
		Frame.Encoding.FullSyncIntervalSeconds = 10.0f; // full syncs only when the test asks
		for (int32 Index = 0; Index < Descriptor->BoneNames.Num(); ++Index)
		{
			const FQuat Rotation(FVector(0.0, 0.0, 1.0), 0.3 * Index + Time);
			Frame.BoneLocalTransforms.Add(FTransform(Rotation, FVector(Index + Time, 2.0 * Time, -Index), FVector::OneVector));
		}
		return Frame;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverResidualGapTest, "Open3DBroadcast.Receiver.Correctness.ResidualGapHoldsUntilFullSync", O3DB_TEST_FLAGS)
bool FO3DReceiverResidualGapTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverResyncTests;
	using FAccessor = FO3DReceiverCorrectnessTestAccessor;

	TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
	Descriptor->BoneNames = { FName(TEXT("root")), FName(TEXT("spine")), FName(TEXT("head")) };
	Descriptor->ParentIndices = { -1, 0, 1 };
	Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);

	FO3DReceiverSourceConfig Config;
	Config.TransportName = FName(TEXT("loopback")); // never started; frames come from the test
	TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Config);
	TSharedRef<FAccessor::FRecorder> Recorder = MakeShared<FAccessor::FRecorder>();
	FAccessor::BindRecorder(*Source, Recorder);
	TSharedRef<ISerializedFrameConsumer> Consumer = FAccessor::MakeConsumer(Source);

	const FString Hero = TEXT("Hero");
	const FName HeroName(*Hero);
	FO3DSenderSerializer Serializer;
	auto Send = [&](int32 Index, bool bDeliver, bool* bOutFullSync = nullptr)
	{
		TArray<uint8> Bytes;
		bool bFullSync = false;
		TestTrue(FString::Printf(TEXT("frame %d serialized"), Index), Serializer.SerializePoseFrameTo(Hero, MakeResidualFrame(Hero, Descriptor, 1.0 + Index / 60.0), Bytes, bFullSync));
		if (bOutFullSync != nullptr)
		{
			*bOutFullSync = bFullSync;
		}
		if (bDeliver)
		{
			Consumer->SubmitFrame(TEXT("fake"), Bytes, FPlatformTime::Seconds());
		}
	};
	// This source's own counter (ADR 0012 item 4), not the process-wide aggregate.
	const auto AwaitingFullSync = [&Source]() { return Source->GetMetricsHandle()->GetCounters().UpdatesAwaitingFullSync.load(); };

	// A full Subject, then residual updates: every frame is pushed.
	constexpr int32 GapIndex = 5;
	for (int32 Index = 0; Index < GapIndex; ++Index)
	{
		Send(Index, true);
	}
	TestEqual(TEXT("Frames before the gap are pushed"), Recorder->CountFrames(HeroName), GapIndex);

	// One residual frame is lost. The gate waits for it until its window (16 frames) fills, then
	// releases the rest; their residual updates are dropped, so nothing is pushed.
	Send(GapIndex, false);
	const uint64 AwaitingBefore = AwaitingFullSync();
	constexpr int32 FullSyncIndex = 30;
	for (int32 Index = GapIndex + 1; Index < FullSyncIndex; ++Index)
	{
		Send(Index, true);
	}
	TestEqual(TEXT("Nothing is pushed between the gap and the next full Subject"), Recorder->CountFrames(HeroName), GapIndex);
	TestEqual(TEXT("Every released update after the gap was dropped and counted"), AwaitingFullSync() - AwaitingBefore, (uint64)(FullSyncIndex - GapIndex - 1));

	// The next full Subject resynchronises; pushes resume.
	Serializer.RequestFullSync(Hero);
	bool bFullSync = false;
	Send(FullSyncIndex, true, &bFullSync);
	TestTrue(TEXT("The requested frame is a full Subject"), bFullSync);
	constexpr int32 After = 10;
	for (int32 Index = FullSyncIndex + 1; Index <= FullSyncIndex + After; ++Index)
	{
		Send(Index, true);
	}
	TestEqual(TEXT("The full Subject and the updates after it are pushed"), Recorder->CountFrames(HeroName), GapIndex + 1 + After);
	TestEqual(TEXT("No update after the resync was dropped"), AwaitingFullSync() - AwaitingBefore, (uint64)(FullSyncIndex - GapIndex - 1));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
