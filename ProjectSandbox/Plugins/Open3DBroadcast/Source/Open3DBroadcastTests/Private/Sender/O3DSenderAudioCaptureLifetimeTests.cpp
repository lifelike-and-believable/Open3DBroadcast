// Copyright Lifelike & Believable. All Rights Reserved.

// WP-S5 (SND-6): the audio capture component publishes an immutable parameter snapshot that
// capture threads read; they never touch the UObject. This test swaps sinks, labels and gain on
// the game thread 1,000 times while a fake capture thread pushes frames.
// SND-7 (unregister from the submix the tap was registered on) needs a live audio mixer and
// is not covered here; see the ADR 0007 addendum.

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderAudioCaptureComponent.h"
#include "O3DSenderInterface.h"

#include "Misc/AutomationTest.h"
#include "O3DTestHarness.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#include <atomic>

namespace
{
	class FRecordingSink final : public IO3DSenderAudioSink
	{
	public:
		explicit FRecordingSink(FString InExpectedLabel)
			: ExpectedLabel(MoveTemp(InExpectedLabel))
		{
		}

		virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
		{
			Calls.fetch_add(1);
			if (StreamLabel != ExpectedLabel || NumChannels != 1 || SampleRate != 48000 || !Interleaved || NumFrames <= 0)
			{
				Violations.fetch_add(1);
			}
			return true;
		}

		const FString ExpectedLabel;
		std::atomic<int64> Calls{0};
		std::atomic<int64> Violations{0};
	};

	class FFakeCaptureThread final : public FRunnable
	{
	public:
		explicit FFakeCaptureThread(UO3DSenderAudioCaptureComponent* InComponent)
			: Component(InComponent)
		{
			// Stereo 44.1 kHz in, so every buffer goes through the mix/resample path into
			// the producer's own scratch.
			Samples.Init(0.1f, 441 * 2);
			Thread = FRunnableThread::Create(this, TEXT("O3D_FakeCaptureThread"));
		}

		virtual ~FFakeCaptureThread() override
		{
			Join();
		}

		void Join()
		{
			bStop.store(true);
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
		}

		virtual uint32 Run() override
		{
			double Clock = 0.0;
			while (!bStop.load())
			{
				// PushFrames is the any-thread entry point; it reads only the snapshot.
				Component->PushFrames(Samples.GetData(), 441, 2, 44100, Clock);
				Clock += 0.01;
				Pushed.fetch_add(1);
			}
			return 0;
		}

		std::atomic<int64> Pushed{0};

	private:
		UO3DSenderAudioCaptureComponent* Component;
		TArray<float> Samples;
		std::atomic<bool> bStop{false};
		FRunnableThread* Thread = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderCaptureParamsLifetimeTest, "Open3DBroadcast.Sender.Lifetime.CaptureParamsSnapshot", O3DB_TEST_FLAGS)
bool FO3DSenderCaptureParamsLifetimeTest::RunTest(const FString& Parameters)
{
	UO3DSenderAudioCaptureComponent* Component = NewObject<UO3DSenderAudioCaptureComponent>();
	Component->AddToRoot();
	Component->CaptureMode = EO3DSenderCaptureMode::Mix;
	Component->Config.SampleRate = 48000;
	Component->Config.NumChannels = 1;

	TSharedRef<FRecordingSink, ESPMode::ThreadSafe> Hero = MakeShared<FRecordingSink, ESPMode::ThreadSafe>(TEXT("Hero"));
	TSharedRef<FRecordingSink, ESPMode::ThreadSafe> Villain = MakeShared<FRecordingSink, ESPMode::ThreadSafe>(TEXT("Villain"));

	Component->SetAudioSink(Hero, TEXT("Hero"));
	{
		FFakeCaptureThread Capture(Component);
		for (int32 Cycle = 0; Cycle < 1000; ++Cycle)
		{
			switch (Cycle % 4)
			{
			case 0:
				Component->SetAudioSink(Villain, TEXT("Villain"));
				break;
			case 1:
				Component->Config.GameGain = (Cycle % 8 == 1) ? 0.5f : 1.0f;
				Component->RefreshCaptureParams();
				break;
			case 2:
				Component->SetAudioSink(nullptr, FString());
				break;
			default:
				// No world here, so this only rebuilds parameters (no mixer, no mic in Mix mode).
				Component->StartCaptureWithMode(EO3DSenderCaptureMode::Mix);
				Component->SetAudioSink(Hero, TEXT("Hero"));
				break;
			}
			FPlatformProcess::YieldThread();
		}
		Capture.Join();
		AddInfo(FString::Printf(TEXT("Fake capture thread pushed %lld buffers"), Capture.Pushed.load()));
	}

	TestEqual(TEXT("Hero sink only ever saw its own label and target format"), Hero->Violations.load(), static_cast<int64>(0));
	TestEqual(TEXT("Villain sink only ever saw its own label and target format"), Villain->Violations.load(), static_cast<int64>(0));

	// Deterministic: once the sink is cleared on the game thread, no later buffer reaches it.
	Component->SetAudioSink(nullptr, FString());
	const int64 HeroBefore = Hero->Calls.load();
	const float Mono[4] = {0.f, 0.f, 0.f, 0.f};
	Component->PushFrames(Mono, 4, 1, 48000, 0.0);
	TestEqual(TEXT("Cleared sink receives nothing"), Hero->Calls.load(), HeroBefore);

	Component->SetAudioSink(Hero, TEXT("Hero"));
	Component->PushFrames(Mono, 4, 1, 48000, 0.0);
	TestEqual(TEXT("Rebound sink receives the next buffer"), Hero->Calls.load(), HeroBefore + 1);

	Component->SetAudioSink(nullptr, FString());
	Component->RemoveFromRoot();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
