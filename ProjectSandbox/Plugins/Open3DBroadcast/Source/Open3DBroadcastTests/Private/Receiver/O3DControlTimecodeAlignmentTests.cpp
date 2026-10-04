// Copyright Lifelike & Believable. All Rights Reserved.

// RCV-8, ADR 0013 PR 3b: control alignment in LiveLink Timecode mode. With the sender's timecode
// on the mocap frames and an engine timecode, the sender time of the pose LiveLink presents is
// the newest frame's SubjectList.time less how far its timecode is ahead of LiveLink's read time
// (the engine timecode minus TimecodeFrameOffset frames). Without the sender's timecode, or with
// no engine timecode, nothing is held (false), as before.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "O3DHelpers.h"
#include "O3DReceiverSource.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSenderComponent.h"
#include "O3DSenderSerializer.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <string>
#include <vector>

namespace O3DControlTimecodeAlignmentTests
{
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

	/** Delivers NumFrames frames of Hero at 24 fps sender time 1.0 + i/24, with scene time frame 1000 + i when bTimecode. */
	bool Deliver(FAutomationTestBase& Test, const TSharedRef<FO3DReceiverSource>& Source, bool bTimecode, int32 NumFrames)
	{
		TSharedRef<FO3DSSkeletonDescriptor> Descriptor = MakeShared<FO3DSSkeletonDescriptor>();
		Descriptor->BoneNames = { FName(TEXT("root")) };
		Descriptor->ParentIndices = { -1 };
		Descriptor->Hash = O3DHelpers::HashNamesAndParents(Descriptor->BoneNames, Descriptor->ParentIndices);
		TSharedRef<FO3DReceiverCorrectnessTestAccessor::FRecorder> Recorder = MakeShared<FO3DReceiverCorrectnessTestAccessor::FRecorder>();
		FO3DReceiverCorrectnessTestAccessor::BindRecorder(*Source, Recorder);
		const TSharedRef<ISerializedFrameConsumer> Consumer = FO3DReceiverCorrectnessTestAccessor::MakeConsumer(Source);
		FO3DSenderSerializer Serializer;
		for (int32 Index = 0; Index < NumFrames; ++Index)
		{
			FO3DSPoseFrame Frame;
			Frame.Subject = TEXT("Hero");
			Frame.Descriptor = Descriptor;
			Frame.CaptureTimeSec = 1.0 + Index / 24.0;
			Frame.BoneLocalTransforms.Add(FTransform(FVector(Index, 0.0, 0.0)));
			if (bTimecode)
			{
				Frame.SceneTime = FQualifiedFrameTime(FFrameTime(FFrameNumber(1000 + Index)), FFrameRate(24, 1));
			}
			TArray<uint8> Bytes;
			bool bFullSync = false;
			if (!Test.TestTrue(TEXT("Frame serialized"), Serializer.SerializePoseFrameTo(Frame.Subject, Frame, Bytes, bFullSync)))
			{
				return false;
			}
			Consumer->SubmitFrame(TEXT("fake"), Bytes, FPlatformTime::Seconds());
		}
		return Test.TestEqual(TEXT("Every frame pushed"), Recorder->CountFrames(FName(TEXT("Hero"))), NumFrames);
	}

	TSharedRef<FO3DReceiverSource> MakeTimecodeSource(float TimecodeFrameOffset)
	{
		FO3DReceiverSourceConfig Config;
		Config.TransportName = FName(TEXT("loopback")); // never started; frames come from the test
		TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Config);
		UO3DReceiverSourceSettings* Settings = NewObject<UO3DReceiverSourceSettings>(GetTransientPackage());
		Settings->Mode = ELiveLinkSourceMode::Timecode;
		Settings->BufferSettings.TimecodeFrameOffset = TimecodeFrameOffset;
		Source->InitializeSettings(Settings);
		return Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlTimecodeAlignmentTest, "Open3DBroadcast.Receiver.Control.AlignedInTimecodeMode", O3DB_TEST_FLAGS)
bool FO3DControlTimecodeAlignmentTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlTimecodeAlignmentTests;
	FScopedEngineFrameTime Scoped;
	const std::vector<std::string> Subjects = { "Hero" };
	constexpr int32 NumFrames = 5; // newest: sender time 1 + 4/24 s, timecode frame 1004

	// The engine timecode is two frames behind the newest frame: the pose shown is two frames older.
	{
		const TSharedRef<FO3DReceiverSource> Source = MakeTimecodeSource(0.0f);
		if (!Deliver(*this, Source, true, NumFrames))
		{
			return false;
		}
		FApp::SetCurrentFrameTime(FQualifiedFrameTime(FFrameTime(FFrameNumber(1002)), FFrameRate(24, 1)));
		uint64_t PresentedUs = 0;
		TestTrue(TEXT("Timecode mode with the sender's timecode aligns"), FO3DReceiverSourceTestAccessor::GetPresentedSenderTimeUs(*Source, Subjects, PresentedUs));
		TestTrue(TEXT("Presented sender time: the frame at timecode 1002"), FMath::Abs(static_cast<double>(PresentedUs) - (1.0 + 2.0 / 24.0) * 1.0e6) <= 2.0);

		// The engine timecode at or past the newest frame: the newest frame, never later.
		FApp::SetCurrentFrameTime(FQualifiedFrameTime(FFrameTime(FFrameNumber(1010)), FFrameRate(24, 1)));
		TestTrue(TEXT("Aligns"), FO3DReceiverSourceTestAccessor::GetPresentedSenderTimeUs(*Source, Subjects, PresentedUs));
		TestTrue(TEXT("Clamped to the newest frame"), FMath::Abs(static_cast<double>(PresentedUs) - (1.0 + 4.0 / 24.0) * 1.0e6) <= 2.0);

		// No engine timecode: nothing is held.
		FApp::InvalidateCurrentFrameTime();
		TestFalse(TEXT("No engine timecode: no alignment"), FO3DReceiverSourceTestAccessor::GetPresentedSenderTimeUs(*Source, Subjects, PresentedUs));
	}

	// TimecodeFrameOffset reads one frame further back.
	{
		const TSharedRef<FO3DReceiverSource> Source = MakeTimecodeSource(1.0f);
		if (!Deliver(*this, Source, true, NumFrames))
		{
			return false;
		}
		FApp::SetCurrentFrameTime(FQualifiedFrameTime(FFrameTime(FFrameNumber(1002)), FFrameRate(24, 1)));
		uint64_t PresentedUs = 0;
		TestTrue(TEXT("Aligns with a frame offset"), FO3DReceiverSourceTestAccessor::GetPresentedSenderTimeUs(*Source, Subjects, PresentedUs));
		TestTrue(TEXT("One frame further back"), FMath::Abs(static_cast<double>(PresentedUs) - (1.0 + 1.0 / 24.0) * 1.0e6) <= 2.0);
	}

	// Mocap without the sender's timecode: nothing is held, as before.
	{
		const TSharedRef<FO3DReceiverSource> Source = MakeTimecodeSource(0.0f);
		if (!Deliver(*this, Source, false, NumFrames))
		{
			return false;
		}
		FApp::SetCurrentFrameTime(FQualifiedFrameTime(FFrameTime(FFrameNumber(1002)), FFrameRate(24, 1)));
		uint64_t PresentedUs = 0;
		TestFalse(TEXT("No sender timecode: no alignment"), FO3DReceiverSourceTestAccessor::GetPresentedSenderTimeUs(*Source, Subjects, PresentedUs));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
