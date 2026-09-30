// Copyright Lifelike & Believable. All Rights Reserved.

// WP-S10 sender audio:
// - SND-16: the audio stream label is the pose subject name (sanitized or generated), and it
//   follows the subject name when that changes.
// - SND-21: the capture path resamples statefully, so buffer lengths add up to the exact output
//   rate and every buffer reaches the sink in the target format.
// SND-28 (the submix tap is registered only while a sink is bound) needs a live audio mixer and
// is not covered here, like SND-7 in O3DSenderAudioCaptureLifetimeTests.cpp.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DSenderAudioCaptureComponent.h"
#include "O3DSenderComponent.h"
#include "O3DSenderInterface.h"
#include "Testing/O3DSenderTesting.h"
#include "UObject/Package.h"

namespace O3DSenderAudioStreamTests
{
	/** Records what the capture component submits. Called on the test thread only. */
	class FRecordingSink final : public IO3DSenderAudioSink
	{
	public:
		virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
		{
			++Calls;
			LastLabel = StreamLabel;
			TotalFrames += NumFrames;
			if (!Interleaved || NumFrames <= 0 || NumChannels != ExpectedChannels || SampleRate != ExpectedSampleRate)
			{
				++FormatViolations;
			}
			if (Calls > 1 && TimestampSec < LastTimestamp)
			{
				++TimestampRegressions;
			}
			LastTimestamp = TimestampSec;
			return true;
		}

		int32 ExpectedChannels = 1;
		int32 ExpectedSampleRate = 48000;
		int32 Calls = 0;
		int64 TotalFrames = 0;
		int32 FormatViolations = 0;
		int32 TimestampRegressions = 0;
		double LastTimestamp = 0.0;
		FString LastLabel;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioLabelMatchesSubjectTest, "Open3DBroadcast.Sender.Audio.StreamLabelMatchesSubject", O3DB_TEST_FLAGS)
bool FO3DSenderAudioLabelMatchesSubjectTest::RunTest(const FString& Parameters)
{
	using O3DSenderAudioStreamTests::FRecordingSink;

	UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(GetTransientPackage());
	UO3DSenderAudioCaptureComponent* Capture = NewObject<UO3DSenderAudioCaptureComponent>(GetTransientPackage());
	Sender->AddToRoot();
	Capture->AddToRoot();
	Capture->Config.SampleRate = 48000;
	Capture->Config.NumChannels = 1;

	Sender->bEnableAudio = true;
	Sender->SubjectName = TEXT("My Hero!"); // unsanitized: the space and '!' are not allowed
	FO3DSenderComponentTestAccess::SetAudioCaptureComponent(*Sender, Capture);

	TSharedRef<FRecordingSink, ESPMode::ThreadSafe> Sink = MakeShared<FRecordingSink, ESPMode::ThreadSafe>();
	Capture->SetAudioSink(Sink, TEXT("stale-label"));

	// Resolving the pose subject name pushes it to the audio component.
	FO3DSenderComponentTestAccess::EnsureSubjectNameCached(*Sender);
	const FString PoseSubject = FO3DSenderComponentTestAccess::CreateFrameShell(*Sender, 0.0).Subject;
	TestEqual(TEXT("Pose subject is the sanitized SubjectName"), PoseSubject, FString(TEXT("My_Hero")));

	const float Mono[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
	Capture->PushFrames(Mono, 4, 1, 48000, 1.0);
	TestEqual(TEXT("Audio label equals the pose subject"), Sink->LastLabel, PoseSubject);

	// Renaming the subject renames the audio stream too.
	Sender->SubjectName = TEXT("Villain");
	FO3DSenderComponentTestAccess::EnsureSubjectNameCached(*Sender);
	Capture->PushFrames(Mono, 4, 1, 48000, 2.0);
	TestEqual(TEXT("Audio label follows a subject rename"), Sink->LastLabel, FString(TEXT("Villain")));
	TestEqual(TEXT("Both buffers were delivered"), Sink->Calls, 2);

	FO3DSenderComponentTestAccess::SetAudioCaptureComponent(*Sender, nullptr);
	Capture->SetAudioSink(nullptr, FString());
	Capture->RemoveFromRoot();
	Sender->RemoveFromRoot();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioResampleContinuityTest, "Open3DBroadcast.Sender.Audio.ResampledBuffersAddUp", O3DB_TEST_FLAGS)
bool FO3DSenderAudioResampleContinuityTest::RunTest(const FString& Parameters)
{
	using O3DSenderAudioStreamTests::FRecordingSink;

	UO3DSenderAudioCaptureComponent* Capture = NewObject<UO3DSenderAudioCaptureComponent>(GetTransientPackage());
	Capture->AddToRoot();
	Capture->Config.SampleRate = 48000;
	Capture->Config.NumChannels = 1;

	TSharedRef<FRecordingSink, ESPMode::ThreadSafe> Sink = MakeShared<FRecordingSink, ESPMode::ThreadSafe>();
	Capture->SetAudioSink(Sink, TEXT("hero"));

	// 2 s of 44.1 kHz stereo in 441-frame buffers: mixed to mono and resampled to 48 kHz. The
	// old resampler rounded each buffer independently.
	TArray<float> Buffer;
	Buffer.SetNumUninitialized(441 * 2);
	for (int32 Index = 0; Index < Buffer.Num(); ++Index)
	{
		Buffer[Index] = 0.25f * FMath::Sin(static_cast<float>(Index) * 0.05f);
	}
	const int32 NumBuffers = 200;
	for (int32 BufferIndex = 0; BufferIndex < NumBuffers; ++BufferIndex)
	{
		Capture->PushFrames(Buffer.GetData(), 441, 2, 44100, BufferIndex * 0.01);
	}

	TestEqual(TEXT("Every buffer reached the sink in the target format"), Sink->FormatViolations, 0);
	TestEqual(TEXT("Timestamps never go backwards"), Sink->TimestampRegressions, 0);
	TestTrue(FString::Printf(TEXT("2 s of input gives 2 s of output (%lld frames)"), Sink->TotalFrames), FMath::Abs(Sink->TotalFrames - static_cast<int64>(96000)) <= 1);

	Capture->SetAudioSink(nullptr, FString());
	Capture->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
