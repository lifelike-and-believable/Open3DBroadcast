// Copyright Lifelike & Believable. All Rights Reserved.

// ADR 0012 item 4 (SHR-38) through the real receiver: two receiver sources fed different numbers
// of frames each count their own in their metrics handle, both are listed by the default
// context, and the default aggregate grows by the sum. Frames come from the UE serializer and are
// delivered to each source's serialized-frame consumer; no transport is started.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "O3DReceiverSource.h"
#include "O3DRuntimeContext.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DSerializedFrameConsumer.h"

namespace O3DReceiverMetricsPerSourceTests
{
	FO3DSPoseFrame MakeFrame(const FString& Subject, const TSharedPtr<const FO3DSSkeletonDescriptor>& Descriptor, double Time)
	{
		FO3DSPoseFrame Frame;
		Frame.Subject = Subject;
		Frame.Descriptor = Descriptor;
		Frame.CaptureTimeSec = Time;
		for (int32 Index = 0; Index < Descriptor->BoneNames.Num(); ++Index)
		{
			Frame.BoneLocalTransforms.Add(FTransform(FQuat(FVector(0.0, 0.0, 1.0), 0.1 * Index + Time), FVector(Index + Time, 0.0, 0.0), FVector::OneVector));
		}
		return Frame;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverMetricsPerSourceTest, "Open3DBroadcast.Receiver.Metrics.EachSourceCountsItsOwn", O3DB_TEST_FLAGS)
bool FO3DReceiverMetricsPerSourceTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverMetricsPerSourceTests;
	using FAccessor = FO3DReceiverCorrectnessTestAccessor;

	TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
	Descriptor->BoneNames = { FName(TEXT("root")), FName(TEXT("spine")) };
	Descriptor->ParentIndices = { -1, 0 };
	Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);

	FO3DReceiverSourceConfig Config;
	Config.TransportName = FName(TEXT("loopback")); // never started; frames come from the test
	const TSharedRef<FO3DReceiverSource> SourceA = MakeShared<FO3DReceiverSource>(Config);
	const TSharedRef<FO3DReceiverSource> SourceB = MakeShared<FO3DReceiverSource>(Config);
	TSharedRef<FAccessor::FRecorder> RecorderA = MakeShared<FAccessor::FRecorder>();
	TSharedRef<FAccessor::FRecorder> RecorderB = MakeShared<FAccessor::FRecorder>();
	FAccessor::BindRecorder(*SourceA, RecorderA);
	FAccessor::BindRecorder(*SourceB, RecorderB);
	const TSharedRef<ISerializedFrameConsumer> ConsumerA = FAccessor::MakeConsumer(SourceA);
	const TSharedRef<ISerializedFrameConsumer> ConsumerB = FAccessor::MakeConsumer(SourceB);

	FO3DPerformanceMetrics& Default = FO3DRuntimeContext::Default()->GetMetrics();
	const TArray<FO3DReceiverMetricsHandleRef> Listed = Default.GetReceiverHandles();
	TestTrue(TEXT("The default context lists A's handle"), Listed.Contains(SourceA->GetMetricsHandle()));
	TestTrue(TEXT("The default context lists B's handle"), Listed.Contains(SourceB->GetMetricsHandle()));
	TestTrue(TEXT("The sources have separate handles"), SourceA->GetMetricsHandle() != SourceB->GetMetricsHandle());
	const uint64 AggregateBefore = Default.GetReceiverMetrics().FramesReceived.load();

	auto Deliver = [this, &Descriptor](const TSharedRef<ISerializedFrameConsumer>& Consumer, const FString& Subject, int32 Count)
	{
		FO3DSenderSerializer Serializer;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			TArray<uint8> Bytes;
			bool bFullSync = false;
			TestTrue(FString::Printf(TEXT("%s frame %d serialized"), *Subject, Index), Serializer.SerializePoseFrameTo(Subject, MakeFrame(Subject, Descriptor, 1.0 + Index / 60.0), Bytes, bFullSync));
			Consumer->SubmitFrame(TEXT("fake"), Bytes, FPlatformTime::Seconds());
		}
	};
	constexpr int32 FramesA = 3;
	constexpr int32 FramesB = 5;
	Deliver(ConsumerA, TEXT("HeroA"), FramesA);
	Deliver(ConsumerB, TEXT("HeroB"), FramesB);
	TestEqual(TEXT("A pushed its frames"), RecorderA->CountFrames(FName(TEXT("HeroA"))), FramesA);
	TestEqual(TEXT("B pushed its frames"), RecorderB->CountFrames(FName(TEXT("HeroB"))), FramesB);

	const FO3DReceiverCounters& A = SourceA->GetMetricsHandle()->GetCounters();
	const FO3DReceiverCounters& B = SourceB->GetMetricsHandle()->GetCounters();
	TestEqual(TEXT("A: frames received"), A.FramesReceived.load(), (uint64)FramesA);
	TestEqual(TEXT("A: frames applied"), A.FramesApplied.load(), (uint64)FramesA);
	TestEqual(TEXT("B: frames received"), B.FramesReceived.load(), (uint64)FramesB);
	TestEqual(TEXT("B: frames applied"), B.FramesApplied.load(), (uint64)FramesB);
	TestTrue(TEXT("A: bytes deserialized"), A.BytesDeserialized.load() > 0);
	TestEqual(TEXT("The default aggregate grew by the sum"), Default.GetReceiverMetrics().FramesReceived.load() - AggregateBefore, (uint64)(FramesA + FramesB));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
