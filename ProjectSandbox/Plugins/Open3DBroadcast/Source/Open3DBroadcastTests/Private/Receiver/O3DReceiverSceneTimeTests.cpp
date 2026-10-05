// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// RCV-8, ADR 0013 PR 3 through the real receiver: frames from the UE serializer, delivered to
// FO3DReceiverSource's serialized-frame consumer, reach LiveLink (the test push hook) with a
// SceneTime. A frame with the sender's timecode carries it exactly; a subject keeps its first
// rate and converts later ones; frames without one, with an engine timecode set, get increasing
// derived values; with no timecode anywhere, SceneTime is left at LiveLink's default.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "O3DHelpers.h"
#include "O3DReceiverSource.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DSerializedFrameConsumer.h"

namespace O3DReceiverSceneTimeTests
{
	using FAccessor = FO3DReceiverCorrectnessTestAccessor;

	struct FHarness
	{
		TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
		TSharedPtr<FO3DReceiverSource> Source;
		TSharedRef<FAccessor::FRecorder> Recorder = MakeShared<FAccessor::FRecorder>();
		TSharedPtr<ISerializedFrameConsumer> Consumer;
		FO3DSenderSerializer Serializer;

		FHarness()
		{
			Descriptor->BoneNames = { FName(TEXT("root")), FName(TEXT("spine")) };
			Descriptor->ParentIndices = { -1, 0 };
			Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);
			FO3DReceiverSourceConfig Config;
			Config.TransportName = FName(TEXT("loopback")); // never started; frames come from the test
			TSharedRef<FO3DReceiverSource> Created = MakeShared<FO3DReceiverSource>(Config);
			FAccessor::BindRecorder(*Created, Recorder);
			Consumer = FAccessor::MakeConsumer(Created);
			Source = Created;
		}

		/** Serializes one frame of Subject, with SceneTime when given, and delivers it. */
		bool Send(FAutomationTestBase& Test, const FString& Subject, double Time, const TOptional<FQualifiedFrameTime>& SceneTime)
		{
			FO3DSPoseFrame Frame;
			Frame.Subject = Subject;
			Frame.Descriptor = Descriptor;
			Frame.CaptureTimeSec = Time;
			Frame.SceneTime = SceneTime;
			for (int32 Index = 0; Index < Descriptor->BoneNames.Num(); ++Index)
			{
				Frame.BoneLocalTransforms.Add(FTransform(FQuat(FVector(0.0, 0.0, 1.0), 0.1 * Index + Time), FVector(Index + Time, 0.0, 0.0), FVector::OneVector));
			}
			TArray<uint8> Bytes;
			bool bFullSync = false;
			if (!Test.TestTrue(TEXT("Frame serialized"), Serializer.SerializePoseFrameTo(Subject, Frame, Bytes, bFullSync)))
			{
				return false;
			}
			Consumer->SubmitFrame(TEXT("fake"), Bytes, FPlatformTime::Seconds());
			return true;
		}

		TArray<TOptional<FQualifiedFrameTime>> SceneTimesOf(FName Subject) const
		{
			TArray<TOptional<FQualifiedFrameTime>> Result;
			for (const FAccessor::FFramePush& Push : Recorder->Frames)
			{
				if (Push.Subject == Subject)
				{
					Result.Add(Push.SceneTime);
				}
			}
			return Result;
		}
	};

	/** Restores FApp's current frame time when a test ends. */
	struct FScopedEngineFrameTime
	{
		const TOptional<FQualifiedFrameTime> Saved = FApp::GetCurrentFrameTime();
		~FScopedEngineFrameTime()
		{
			if (Saved.IsSet())
			{
				FApp::SetCurrentFrameTime(Saved.GetValue());
			}
			else
			{
				FApp::InvalidateCurrentFrameTime();
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSceneTimeSenderTest, "Open3DBroadcast.Receiver.SceneTime.SenderTimecodeThroughTheReceiver", O3DB_TEST_FLAGS)
bool FO3DReceiverSceneTimeSenderTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverSceneTimeTests;
	FScopedEngineFrameTime Scoped;
	FApp::InvalidateCurrentFrameTime(); // the receiver has no timecode; the sender's still arrives

	FHarness Harness;
	const FString Hero = TEXT("Hero");
	constexpr int32 NumFrames = 5;
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		const FQualifiedFrameTime Stamp(FFrameTime(FFrameNumber(1000 + Index), 0.0f), FFrameRate(24, 1));
		if (!Harness.Send(*this, Hero, 1.0 + Index / 24.0, Stamp))
		{
			return false;
		}
	}
	// A frame at another rate: converted to the subject's first rate (LiveLink flushes on a rate change).
	if (!Harness.Send(*this, Hero, 1.0 + NumFrames / 24.0, FQualifiedFrameTime(FFrameTime(FFrameNumber(1255), 0.0f), FFrameRate(30, 1))))
	{
		return false;
	}

	const TArray<TOptional<FQualifiedFrameTime>> Times = Harness.SceneTimesOf(FName(*Hero));
	if (!TestEqual(TEXT("Every frame pushed"), Times.Num(), NumFrames + 1))
	{
		return false;
	}
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		TestTrue(FString::Printf(TEXT("Frame %d: the sender's timecode"), Index), Times[Index].IsSet()
			&& Times[Index]->Time.GetFrame().Value == 1000 + Index && Times[Index]->Rate == FFrameRate(24, 1));
	}
	const TOptional<FQualifiedFrameTime>& Converted = Times[NumFrames];
	TestTrue(TEXT("Other rate: kept at 24 fps"), Converted.IsSet() && Converted->Rate == FFrameRate(24, 1));
	TestTrue(TEXT("Other rate: same instant"), Converted.IsSet() && FMath::IsNearlyEqual(Converted->AsSeconds(), 1255.0 / 30.0, 1.0e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverSceneTimeFallbackTest, "Open3DBroadcast.Receiver.SceneTime.FallbackThroughTheReceiver", O3DB_TEST_FLAGS)
bool FO3DReceiverSceneTimeFallbackTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverSceneTimeTests;
	FScopedEngineFrameTime Scoped;
	const FString Hero = TEXT("Hero");
	const FString Villain = TEXT("Villain");

	// No timecode anywhere: LiveLink's default, as before.
	{
		FApp::InvalidateCurrentFrameTime();
		FHarness Harness;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			if (!Harness.Send(*this, Villain, 1.0 + Index / 60.0, TOptional<FQualifiedFrameTime>()))
			{
				return false;
			}
		}
		for (const TOptional<FQualifiedFrameTime>& Time : Harness.SceneTimesOf(FName(*Villain)))
		{
			TestFalse(TEXT("No timecode anywhere: unset"), Time.IsSet());
		}
	}

	// The receiving engine has a timecode, the sender none: derived, increasing, at the engine's rate.
	FApp::SetCurrentFrameTime(FQualifiedFrameTime(FFrameTime(FFrameNumber(86400 * 24)), FFrameRate(24, 1)));
	FHarness Harness;
	constexpr int32 NumFrames = 8;
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		if (!Harness.Send(*this, Hero, 1.0 + Index / 60.0, TOptional<FQualifiedFrameTime>()))
		{
			return false;
		}
	}
	const TArray<TOptional<FQualifiedFrameTime>> Times = Harness.SceneTimesOf(FName(*Hero));
	if (!TestEqual(TEXT("Every frame pushed"), Times.Num(), NumFrames))
	{
		return false;
	}
	double Previous = -1.0;
	for (int32 Index = 0; Index < NumFrames; ++Index)
	{
		if (!TestTrue(FString::Printf(TEXT("Frame %d: derived SceneTime"), Index), Times[Index].IsSet()))
		{
			return false;
		}
		TestTrue(FString::Printf(TEXT("Frame %d: at the engine's rate"), Index), Times[Index]->Rate == FFrameRate(24, 1));
		TestTrue(FString::Printf(TEXT("Frame %d: after the previous one"), Index), Times[Index]->AsSeconds() > Previous);
		Previous = Times[Index]->AsSeconds();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
