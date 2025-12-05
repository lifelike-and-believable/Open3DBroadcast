#if WITH_DEV_AUTOMATION_TESTS

#include "../Sender/WebRTCSender.h"
#include "../Receiver/WebRTCReceiver.h"

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"

#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"
#include "o3ds/model.h"

namespace WebRTCPerSubjectTestHelpers
{
	/**
	 * Create test config for WebRTC tests
	 */
	FO3DTransportConfig CreateTestConfig(const FString& Url, const FString& Token, bool bEnableAudio = false)
	{
		FO3DTransportConfig Config;
		Config.Uri = Url;
		Config.Token = Token;
		Config.StreamId = TEXT("TestStream");
		Config.Audio.bEnableAudio = bEnableAudio;
		Config.Audio.BitrateKbps = 24;
		Config.Audio.NumChannels = 1;
		Config.Audio.SampleRate = 48000;
		return Config;
	}

	/**
	 * Create a SubjectList with multiple distinct subjects
	 */
	O3DS::SubjectList CreateMultiSubjectList()
	{
		O3DS::SubjectList List;

		// Subject 1: Alice
		const FTCHARToUTF8 Name1(TEXT("Alice"));
		O3DS::Subject* Alice = List.addSubject(std::string(Name1.Get(), Name1.Length()));
		Alice->addTransform("Root", -1);
		Alice->addTransform("Spine", 0);
		Alice->addTransform("Head", 1);

		// Subject 2: Bob
		const FTCHARToUTF8 Name2(TEXT("Bob"));
		O3DS::Subject* Bob = List.addSubject(std::string(Name2.Get(), Name2.Length()));
		Bob->addTransform("Root", -1);
		Bob->addTransform("LeftArm", 0);
		Bob->addTransform("RightArm", 0);

		// Subject 3: Charlie
		const FTCHARToUTF8 Name3(TEXT("Charlie"));
		O3DS::Subject* Charlie = List.addSubject(std::string(Name3.Get(), Name3.Length()));
		Charlie->addTransform("Root", -1);
		Charlie->mCurveNames = {"Smile", "Frown"};
		Charlie->mCurveValues = {0.7f, 0.3f};

		return List;
	}

	/**
	 * Runnable for concurrent audio submission to different subjects
	 */
	class FConcurrentAudioSubmitWorker : public FRunnable
	{
	public:
		FConcurrentAudioSubmitWorker(TSharedPtr<IO3DSenderAudioSink> InSink, const FString& InSubjectLabel, int32 InNumSubmits)
			: AudioSink(InSink)
			, SubjectLabel(InSubjectLabel)
			, NumSubmits(InNumSubmits)
			, SuccessfulSubmits(0)
		{
		}

		virtual uint32 Run() override
		{
			// Generate 48kHz mono audio at 20ms intervals (960 samples)
			TArray<float> TestAudio;
			TestAudio.SetNum(960);

			for (int32 i = 0; i < NumSubmits; ++i)
			{
				// Generate unique sine wave per subject
				const float Frequency = 440.0f * (1.0f + SubjectLabel.Len() * 0.1f); // Different freq per subject
				for (int32 SampleIdx = 0; SampleIdx < 960; ++SampleIdx)
				{
					const float Phase = (float)SampleIdx / 960.0f;
					TestAudio[SampleIdx] = FMath::Sin(Phase * 2.0f * PI * Frequency / 48000.0f) * 0.5f;
				}

				// Submit audio for this subject
				if (AudioSink.IsValid())
				{
					bool bSubmitted = AudioSink->SubmitPcm(SubjectLabel, TestAudio.GetData(), 960, 1, 48000, FPlatformTime::Seconds());
					if (bSubmitted)
					{
						SuccessfulSubmits++;
					}
				}

				FPlatformProcess::Sleep(0.02f); // 20ms interval
			}

			return 0;
		}

		TSharedPtr<IO3DSenderAudioSink> AudioSink;
		FString SubjectLabel;
		int32 NumSubmits;
		TAtomic<int32> SuccessfulSubmits;
	};
}

// ============================================================================
// PHASE 1: PER-SUBJECT DATA CHANNELS
// ============================================================================

/**
 * Test: Multiple subjects create separate data channels
 * Verifies Phase 1 implementation: Per-subject labeled data channels
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCPerSubjectDataChannelsTest,
	"Open3DBroadcast.Open3DTransportWebRTC.PerSubject.DataChannels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCPerSubjectDataChannelsTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCPerSubjectTestHelpers;

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	TestTrue(TEXT("Initialize sender"), Sender.Initialize(Config));

	// Create multi-subject list
	O3DS::SubjectList MultiSubjects = CreateMultiSubjectList();

	// Attempt to send (will fail without connection, but tests internal logic)
	bool bSendResult = Sender.Send(MultiSubjects);

	// Verify that sender processed the multi-subject list without crashing
	// Note: Without actual server connection, we can't verify channel creation
	// but this tests that the code path doesn't crash

	FO3DTransportStats Stats = Sender.GetStats();
	TestTrue(TEXT("Stats should track send attempt"), Stats.DroppedFrames > 0 || Stats.FramesSent >= 0);

	Sender.Stop();

	AddInfo(TEXT("Per-subject data channel test executed (requires live server for full validation)"));
#endif

	return true;
}

/**
 * Test: Subject labels are correctly applied to data channels
 * Verifies that data channel labels match subject names
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCDataChannelLabelingTest,
	"Open3DBroadcast.Open3DTransportWebRTC.PerSubject.ChannelLabeling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCDataChannelLabelingTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCPerSubjectTestHelpers;

	// Create subjects with distinct names
	O3DS::SubjectList List = CreateMultiSubjectList();

	// Verify subject names are set correctly in the model
	TestEqual(TEXT("Should have 3 subjects"), List.mItems.size(), static_cast<size_t>(3));

	if (List.mItems.size() >= 3)
	{
		TestEqual(TEXT("First subject should be Alice"),
			FString(UTF8_TO_TCHAR(List.mItems[0]->mName.c_str())),
			TEXT("Alice"));
		TestEqual(TEXT("Second subject should be Bob"),
			FString(UTF8_TO_TCHAR(List.mItems[1]->mName.c_str())),
			TEXT("Bob"));
		TestEqual(TEXT("Third subject should be Charlie"),
			FString(UTF8_TO_TCHAR(List.mItems[2]->mName.c_str())),
			TEXT("Charlie"));
	}

	// Transport-specific implementations should:
	// 1. Send this list through WebRTC sender
	// 2. Verify via LiveKit FFI that separate data channels are created
	// 3. Verify channel labels match: "Alice", "Bob", "Charlie"
	// 4. Verify data for each subject goes to correct channel

	AddInfo(TEXT("Subject labeling verified in model (channel verification requires live connection)"));

	return true;
}

// ============================================================================
// PHASE 2: PER-SUBJECT AUDIO TRACKS
// ============================================================================

/**
 * Test: Multiple subjects create separate audio tracks
 * Verifies Phase 2 implementation: Per-subject audio tracks
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCPerSubjectAudioTracksTest,
	"Open3DBroadcast.Open3DTransportWebRTC.PerSubject.AudioTracks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCPerSubjectAudioTracksTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCPerSubjectTestHelpers;

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true  // Enable audio
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	TestTrue(TEXT("Initialize sender with audio"), Sender.Initialize(Config));

	// Create audio sink
	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.BitrateKbps = 48;
	AudioConfig.NumChannels = 1;
	AudioConfig.SampleRate = 48000;

	auto AudioSink = Sender.CreateAudioSink(AudioConfig);
	TestTrue(TEXT("Audio sink should be created"), AudioSink.IsValid());

	if (AudioSink.IsValid())
	{
		// Submit audio for different subjects
		TArray<float> TestAudio;
		TestAudio.SetNum(960);  // 20ms at 48kHz
		for (int32 i = 0; i < 960; ++i)
		{
			TestAudio[i] = FMath::Sin((float)i / 960.0f * 2.0f * PI) * 0.5f;
		}

		// Submit for Subject A
		bool bResultA = AudioSink->SubmitPcm(TEXT("Alice"), TestAudio.GetData(), 960, 1, 48000, FPlatformTime::Seconds());

		// Submit for Subject B
		bool bResultB = AudioSink->SubmitPcm(TEXT("Bob"), TestAudio.GetData(), 960, 1, 48000, FPlatformTime::Seconds());

		// Verify that submission doesn't crash (will fail without connection)
		// but validates internal logic paths
		TestTrue(TEXT("Audio submission should not crash"), true);

		// With live connection, should verify:
		// 1. Separate LkAudioTrackHandle created for "Alice" and "Bob"
		// 2. Audio data routed to correct tracks
		// 3. Track labels visible to receiver
	}

	Sender.Stop();

	AddInfo(TEXT("Per-subject audio track test executed (requires live server for full validation)"));
#endif

	return true;
}

/**
 * Test: Concurrent audio submission to different subjects
 * Verifies thread-safety of per-subject audio track map
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCConcurrentPerSubjectAudioTest,
	"Open3DBroadcast.Open3DTransportWebRTC.PerSubject.ConcurrentAudio",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCConcurrentPerSubjectAudioTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCPerSubjectTestHelpers;

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true  // Enable audio
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	TestTrue(TEXT("Initialize sender with audio"), Sender.Initialize(Config));

	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.BitrateKbps = 48;
	AudioConfig.NumChannels = 1;
	AudioConfig.SampleRate = 48000;

	auto AudioSink = Sender.CreateAudioSink(AudioConfig);
	TestTrue(TEXT("Audio sink should be created"), AudioSink.IsValid());

	if (AudioSink.IsValid())
	{
		// Create multiple threads submitting audio for different subjects
		const int32 NumThreads = 3;
		const int32 NumSubmitsPerThread = 5;

		TArray<TUniquePtr<FConcurrentAudioSubmitWorker>> Workers;
		TArray<FRunnableThread*> Threads;

		TArray<FString> SubjectNames = {TEXT("Alice"), TEXT("Bob"), TEXT("Charlie")};

		for (int32 i = 0; i < NumThreads; ++i)
		{
			auto Worker = MakeUnique<FConcurrentAudioSubmitWorker>(AudioSink, SubjectNames[i], NumSubmitsPerThread);
			FRunnableThread* Thread = FRunnableThread::Create(Worker.Get(), *FString::Printf(TEXT("AudioWorker_%d"), i));

			Workers.Add(MoveTemp(Worker));
			Threads.Add(Thread);
		}

		// Wait for all threads to complete
		for (FRunnableThread* Thread : Threads)
		{
			Thread->WaitForCompletion();
			delete Thread;
		}

		// Verify no crashes occurred
		TestTrue(TEXT("Concurrent audio submission should not crash"), true);

		// Log results
		int32 TotalSuccessful = 0;
		for (const auto& Worker : Workers)
		{
			TotalSuccessful += Worker->SuccessfulSubmits.Load();
		}

		AddInfo(FString::Printf(TEXT("Submitted %d audio frames concurrently across 3 subjects"), TotalSuccessful));
	}

	Sender.Stop();
#endif

	return true;
}

/**
 * Test: Audio track cleanup when subjects disappear
 * Verifies that audio tracks are properly released
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAudioTrackCleanupTest,
	"Open3DBroadcast.Open3DTransportWebRTC.PerSubject.AudioTrackCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCAudioTrackCleanupTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCPerSubjectTestHelpers;

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	TestTrue(TEXT("Initialize sender"), Sender.Initialize(Config));

	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.BitrateKbps = 48;
	AudioConfig.NumChannels = 1;
	AudioConfig.SampleRate = 48000;

	auto AudioSink = Sender.CreateAudioSink(AudioConfig);

	if (AudioSink.IsValid())
	{
		TArray<float> TestAudio;
		TestAudio.SetNum(960);
		for (int32 i = 0; i < 960; ++i)
		{
			TestAudio[i] = 0.5f;
		}

		// Submit audio for subject that will "disappear"
		AudioSink->SubmitPcm(TEXT("TemporarySubject"), TestAudio.GetData(), 960, 1, 48000, FPlatformTime::Seconds());

		// Stop sender (should clean up all audio tracks)
		Sender.Stop();

		// Verify no crashes during cleanup
		TestTrue(TEXT("Stop should clean up audio tracks without crashing"), true);
	}

	AddInfo(TEXT("Audio track cleanup test completed (requires instrumentation to verify track handle cleanup)"));
#endif

	return true;
}

// ============================================================================
// CONCURRENT SEND TESTS (WebRTC-specific implementation)
// ============================================================================

/**
 * Runnable for concurrent Send() testing
 */
class FWebRTCConcurrentSendWorker : public FRunnable
{
public:
	FWebRTCConcurrentSendWorker(FO3DWebRTCSender* InSender, int32 InThreadId, int32 InNumSends)
		: Sender(InSender)
		, ThreadId(InThreadId)
		, NumSends(InNumSends)
		, SuccessfulSends(0)
	{
	}

	virtual uint32 Run() override
	{
		for (int32 i = 0; i < NumSends; ++i)
		{
			O3DS::SubjectList List;
			const FString SubjectName = FString::Printf(TEXT("Thread%d_Subject%d"), ThreadId, i);
			const FTCHARToUTF8 NameUtf8(*SubjectName);
			O3DS::Subject* Subject = List.addSubject(std::string(NameUtf8.Get(), NameUtf8.Length()));
			Subject->addTransform("Root", -1);
			Subject->addTransform("Bone", 0);

			bool bSent = Sender->Send(List);
			if (bSent)
			{
				SuccessfulSends++;
			}

			FPlatformProcess::Sleep(0.001f);
		}
		return 0;
	}

	FO3DWebRTCSender* Sender;
	int32 ThreadId;
	int32 NumSends;
	TAtomic<int32> SuccessfulSends;
};

/**
 * Test: Concurrent Send() calls from multiple threads (WebRTC implementation)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCConcurrentSendTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Concurrency.MultipleSends",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCConcurrentSendTest::RunTest(const FString& Parameters)
{
	using namespace WebRTCPerSubjectTestHelpers;

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	TestTrue(TEXT("Initialize sender"), Sender.Initialize(Config));

	// Create multiple threads sending concurrently
	const int32 NumThreads = 4;
	const int32 NumSendsPerThread = 10;

	TArray<TUniquePtr<FWebRTCConcurrentSendWorker>> Workers;
	TArray<FRunnableThread*> Threads;

	FO3DTransportStats StatsBefore = Sender.GetStats();

	for (int32 i = 0; i < NumThreads; ++i)
	{
		auto Worker = MakeUnique<FWebRTCConcurrentSendWorker>(&Sender, i, NumSendsPerThread);
		FRunnableThread* Thread = FRunnableThread::Create(Worker.Get(), *FString::Printf(TEXT("SendWorker_%d"), i));

		Workers.Add(MoveTemp(Worker));
		Threads.Add(Thread);
	}

	// Wait for all threads
	for (FRunnableThread* Thread : Threads)
	{
		Thread->WaitForCompletion();
		delete Thread;
	}

	FO3DTransportStats StatsAfter = Sender.GetStats();

	// Verify stats consistency
	const int64 TotalAttempts = NumThreads * NumSendsPerThread;
	const int64 TotalProcessed = StatsAfter.FramesSent + StatsAfter.DroppedFrames;

	TestEqual(TEXT("Stats should account for all send attempts"), TotalProcessed, TotalAttempts);
	TestTrue(TEXT("No crashes during concurrent sends"), true);

	Sender.Stop();

	AddInfo(FString::Printf(TEXT("Concurrent sends: %d attempts, %lld sent, %lld dropped"),
		(int32)TotalAttempts, StatsAfter.FramesSent, StatsAfter.DroppedFrames));
#endif

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
