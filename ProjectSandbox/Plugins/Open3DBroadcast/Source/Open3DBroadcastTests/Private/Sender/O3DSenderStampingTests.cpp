// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// Sender stamping (ADR 0005 (iv), SND-15, CORE-29): every frame the UE serializer writes carries
// tx_seq, tx_wallclock_us and frame_epoch, so the receiver orders it through the reorder gate
// instead of the legacy timestamp check. Each subject is its own stream (the receiver keys streams
// by subject names), and a serializer whose caches were cleared (Stop/Start) restarts in a newer
// epoch, so its restarted counter is not dropped as stale.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"
#include "Testing/O3DReceiverTesting.h"

namespace O3DSenderStampingTests
{
	FO3DSPoseFrame MakeStampFrame(const FString& Subject, const TSharedPtr<const FO3DSSkeletonDescriptor>& Descriptor, EO3DSenderEncodingMode Mode, double Time)
	{
		FO3DSPoseFrame Frame;
		Frame.Subject = Subject;
		Frame.Descriptor = Descriptor;
		Frame.CaptureTimeSec = Time;
		Frame.Encoding.Mode = Mode;
		for (int32 Index = 0; Index < Descriptor->BoneNames.Num(); ++Index)
		{
			const FQuat Rotation(FVector(0.0, 0.0, 1.0), 0.1 * Index + Time);
			Frame.BoneLocalTransforms.Add(FTransform(Rotation, FVector(Index + Time, 2.0 * Time, -Index), FVector::OneVector));
		}
		return Frame;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderStampingTest, "Open3DBroadcast.Sender.Wire.FramesTakeTheGatedPath", O3DB_TEST_FLAGS)
bool FO3DSenderStampingTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderStampingTests;

	TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
	Descriptor->BoneNames = { FName(TEXT("root")), FName(TEXT("spine")), FName(TEXT("head")) };
	Descriptor->ParentIndices = { -1, 0, 1 };
	Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);

	const FString Hero = TEXT("Hero");
	const FString Villain = TEXT("Villain");
	const TArray<TPair<EO3DSenderEncodingMode, FString>> Modes = {
		{ EO3DSenderEncodingMode::Legacy, TEXT("Legacy") },
		{ EO3DSenderEncodingMode::Residual, TEXT("Residual") },
		{ EO3DSenderEncodingMode::Quantized, TEXT("Quantized") },
	};
	for (const TPair<EO3DSenderEncodingMode, FString>& ModeAndName : Modes)
	{
		const EO3DSenderEncodingMode Mode = ModeAndName.Key;
		const FString& ModeName = ModeAndName.Value;
		FO3DSenderSerializer Serializer;
		FO3DReceiverStreamSchedulerProbe Receiver;
		double Now = 100.0;

		// Push one frame of Subject through the serializer and the receiver's scheduler.
		auto Send = [&](const FString& Subject, double Time)
		{
			TArray<uint8> Bytes;
			bool bFullSync = false;
			const bool bSerialized = Serializer.SerializePoseFrameTo(Subject, MakeStampFrame(Subject, Descriptor, Mode, Time), Bytes, bFullSync);
			TestTrue(FString::Printf(TEXT("%s: %s t=%.3f serialized"), *ModeName, *Subject, Time), bSerialized);
			Now += 0.001;
			TestTrue(FString::Printf(TEXT("%s: %s t=%.3f verifies"), *ModeName, *Subject, Time), Receiver.Push(Subject, Bytes, Time, Now));
		};

		constexpr int32 FramesPerSubject = 6;
		for (int32 Index = 0; Index < FramesPerSubject; ++Index)
		{
			Send(Hero, 1.0 + Index / 60.0);
			Send(Villain, 1.0 + Index / 60.0);
		}

		TestEqual(FString::Printf(TEXT("%s: every frame released"), *ModeName), Receiver.Released.Num(), 2 * FramesPerSubject);
		TestEqual(FString::Printf(TEXT("%s: one stream per subject"), *ModeName), Receiver.GetNumStreams(), 2);
		TMap<FString, uint64> ExpectedSeq;
		for (const FO3DReceiverStreamSchedulerProbe::FReleased& Released : Receiver.Released)
		{
			uint64& Seq = ExpectedSeq.FindOrAdd(Released.Label, 0);
			++Seq;
			TestTrue(FString::Printf(TEXT("%s: %s seq %llu took the gated path"), *ModeName, *Released.Label, Seq), Released.bGated);
			TestEqual(FString::Printf(TEXT("%s: %s frames count 1, 2, 3..."), *ModeName, *Released.Label), Released.Seq, Seq);
		}

		// Stop/Start within the same second: the caches (and with them each subject's writer) are
		// cleared, so the counter restarts at 1 in a newer epoch, and the gate takes it as a
		// restart rather than dropping the frames as duplicates of seq 1..3.
		Serializer.ClearAllCaches();
		Receiver.Released.Reset();
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Send(Hero, 2.0 + Index / 60.0);
		}
		TestEqual(FString::Printf(TEXT("%s: frames after a restart released"), *ModeName), Receiver.Released.Num(), 3);
		for (int32 Index = 0; Index < Receiver.Released.Num(); ++Index)
		{
			TestTrue(FString::Printf(TEXT("%s: restart frame %d gated"), *ModeName, Index), Receiver.Released[Index].bGated);
			TestEqual(FString::Printf(TEXT("%s: restart frame %d seq"), *ModeName, Index), Receiver.Released[Index].Seq, (uint64)(Index + 1));
		}
		TestEqual(FString::Printf(TEXT("%s: the restart stays on the subject's stream"), *ModeName), Receiver.GetNumStreams(), 2);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
