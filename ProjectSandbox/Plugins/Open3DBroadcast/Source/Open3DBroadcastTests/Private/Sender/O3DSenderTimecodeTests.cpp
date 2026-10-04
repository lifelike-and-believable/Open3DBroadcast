// Copyright Lifelike & Believable. All Rights Reserved.

// RCV-8, ADR 0013 PR 2: the sender stamps its engine timecode. The component reads
// FApp::GetCurrentFrameTime() into each frame it samples (set only while a timecode provider is
// synchronized; the test sets it directly, as UEngine::UpdateTimecode does each tick), and the
// serializer writes it as SubjectList.scene_time in every encoding, full syncs and updates alike.
// Without one, frames carry none.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/QualifiedFrameTime.h"
#include "Misc/ScopeExit.h"
#include "O3DHelpers.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"
#include "Testing/O3DSenderTesting.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/receiver_streams.h"
THIRD_PARTY_INCLUDES_END

namespace O3DSenderTimecodeTests
{
	FO3DSPoseFrame MakeFrame(const FString& Subject, const TSharedPtr<const FO3DSSkeletonDescriptor>& Descriptor, EO3DSenderEncodingMode Mode, double Time)
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

	bool Peek(const TArray<uint8>& Bytes, O3DS::PacketMeta& OutMeta)
	{
		return O3DS::PeekPacketMeta(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num()), OutMeta);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTimecodeShellTest, "Open3DBroadcast.Sender.Timecode.SampledWithTheFrame", O3DB_TEST_FLAGS)
bool FO3DSenderTimecodeShellTest::RunTest(const FString& Parameters)
{
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

	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());

	// No synchronized timecode provider: the frame carries none.
	FApp::InvalidateCurrentFrameTime();
	const FO3DSPoseFrame Without = FO3DSenderComponentTestAccess::CreateFrameShell(*Component, 1.0);
	TestFalse(TEXT("No engine timecode: none on the frame"), Without.SceneTime.IsSet());

	// A synchronized provider: the frame carries the engine timecode of its tick.
	const FQualifiedFrameTime EngineTime(FFrameTime(FFrameNumber(123456), 0.5f), FFrameRate(30000, 1001));
	FApp::SetCurrentFrameTime(EngineTime);
	const FO3DSPoseFrame With = FO3DSenderComponentTestAccess::CreateFrameShell(*Component, 2.0);
	if (!TestTrue(TEXT("Engine timecode: on the frame"), With.SceneTime.IsSet()))
	{
		return false;
	}
	TestEqual(TEXT("Frame number"), With.SceneTime->Time.GetFrame().Value, 123456);
	TestEqual(TEXT("Sub-frame"), With.SceneTime->Time.GetSubFrame(), 0.5f);
	TestEqual(TEXT("Rate numerator"), With.SceneTime->Rate.Numerator, 30000);
	TestEqual(TEXT("Rate denominator"), With.SceneTime->Rate.Denominator, 1001);

	// Reset() clears it, so a pooled frame never carries a stale timecode.
	FO3DSPoseFrame Reused = With;
	Reused.Reset();
	TestFalse(TEXT("Reset clears the timecode"), Reused.SceneTime.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTimecodeWireTest, "Open3DBroadcast.Sender.Timecode.OnTheWireInEveryEncoding", O3DB_TEST_FLAGS)
bool FO3DSenderTimecodeWireTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderTimecodeTests;

	TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
	Descriptor->BoneNames = { FName(TEXT("root")), FName(TEXT("spine")) };
	Descriptor->ParentIndices = { -1, 0 };
	Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);

	const FString Hero = TEXT("Hero");
	const TArray<TPair<EO3DSenderEncodingMode, FString>> Modes = {
		{ EO3DSenderEncodingMode::Legacy, TEXT("Legacy") },
		{ EO3DSenderEncodingMode::Residual, TEXT("Residual") },
		{ EO3DSenderEncodingMode::Quantized, TEXT("Quantized") },
	};
	for (const TPair<EO3DSenderEncodingMode, FString>& ModeAndName : Modes)
	{
		const FString& ModeName = ModeAndName.Value;
		FO3DSenderSerializer Serializer;
		bool bSawUpdate = false;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FO3DSPoseFrame Frame = MakeFrame(Hero, Descriptor, ModeAndName.Key, 1.0 + Index / 60.0);
			const bool bStamp = Index != 2; // frame 2 is sampled without a timecode
			if (bStamp)
			{
				Frame.SceneTime = FQualifiedFrameTime(FFrameTime(FFrameNumber(1000 + Index), 0.25f), FFrameRate(24, 1));
			}
			TArray<uint8> Bytes;
			bool bFullSync = false;
			if (!TestTrue(FString::Printf(TEXT("%s: frame %d serialized"), *ModeName, Index), Serializer.SerializePoseFrameTo(Hero, Frame, Bytes, bFullSync)))
			{
				return false;
			}
			bSawUpdate |= !bFullSync;
			O3DS::PacketMeta Meta;
			if (!TestTrue(FString::Printf(TEXT("%s: frame %d verifies"), *ModeName, Index), Peek(Bytes, Meta)))
			{
				return false;
			}
			TestEqual(FString::Printf(TEXT("%s: frame %d timecode present"), *ModeName, Index), Meta.has_scene_time, bStamp);
			if (bStamp && Meta.has_scene_time)
			{
				TestEqual(FString::Printf(TEXT("%s: frame %d frame number"), *ModeName, Index), Meta.scene_time.frame, 1000 + Index);
				TestEqual(FString::Printf(TEXT("%s: frame %d sub-frame"), *ModeName, Index), Meta.scene_time.subframe, 0.25f);
				TestEqual(FString::Printf(TEXT("%s: frame %d rate"), *ModeName, Index), Meta.scene_time.rate_numerator, 24);
				TestEqual(FString::Printf(TEXT("%s: frame %d rate denominator"), *ModeName, Index), Meta.scene_time.rate_denominator, 1);
			}
		}
		if (ModeAndName.Key != EO3DSenderEncodingMode::Legacy)
		{
			TestTrue(FString::Printf(TEXT("%s: updates were covered, not only full syncs"), *ModeName), bSawUpdate);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
