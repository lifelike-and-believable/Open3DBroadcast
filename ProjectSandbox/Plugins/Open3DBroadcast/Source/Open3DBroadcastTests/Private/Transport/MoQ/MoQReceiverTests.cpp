#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DTransportTypes.h"
#include "Receiver/MoQReceiver.h"
#include "SerializedFrameConsumerRegistry.h"

#if O3D_WITH_TRANSPORT_MOQ

// Mock consumer for testing
class FMoQTestFrameConsumer : public ISerializedFrameConsumer
{
public:
	virtual void SubmitFrame(const FString& Subject, const TArray<uint8>& Buffer, double TimestampSeconds) override
	{
		FScopeLock Lock(&Mutex);
		ReceivedFrames++;
		LastSubject = Subject;
		LastPayloadSize = Buffer.Num();
		LastTimestamp = TimestampSeconds;
	}

	int32 GetReceivedFrames() const
	{
		FScopeLock Lock(&Mutex);
		return ReceivedFrames;
	}

	FString GetLastSubject() const
	{
		FScopeLock Lock(&Mutex);
		return LastSubject;
	}

	int32 GetLastPayloadSize() const
	{
		FScopeLock Lock(&Mutex);
		return LastPayloadSize;
	}

private:
	mutable FCriticalSection Mutex;
	int32 ReceivedFrames = 0;
	FString LastSubject;
	int32 LastPayloadSize = 0;
	double LastTimestamp = 0.0;
};

// Test: Receiver requires URI
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverRequiresUriTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.RequiresUri", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverRequiresUriTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");

	AddExpectedError(TEXT("MoQ receiver configuration invalid"), EAutomationExpectedMessageFlags::Contains, 1);

	TestFalse(TEXT("Initialize should fail when relay URI is missing"), Receiver.Initialize(Config));
	return true;
}

// Test: Receiver initializes with valid config
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverInitializeSuccessTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.Initialize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverInitializeSuccessTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("session/testTrack");

	TestTrue(TEXT("Initialize should succeed with valid relay URI"), Receiver.Initialize(Config));

	// Ensure Stop is safe to call immediately after initialization
	Receiver.Stop();
	return true;
}

// Test: Stop is idempotent
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverStopIsIdempotentTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.StopIdempotent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverStopIsIdempotentTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/idempotent");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config));

	// Multiple Stop calls should not crash
	Receiver.Stop();
	Receiver.Stop();
	Receiver.Stop();

	return true;
}

// Test: Initialize can be called twice
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverInitializeCalledTwiceTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.InitializeTwice", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverInitializeCalledTwiceTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/reinit");

	TestTrue(TEXT("First Initialize should succeed"), Receiver.Initialize(Config));

	// Calling Initialize again should succeed (replaces config)
	Config.StreamId = TEXT("test/reinit2");
	TestTrue(TEXT("Second Initialize should succeed"), Receiver.Initialize(Config));

	Receiver.Stop();
	return true;
}

// Test: GetStats before start
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverGetStatsBeforeStartTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.GetStatsBeforeStart", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverGetStatsBeforeStartTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/stats");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config));

	const FO3DTransportStats Stats = Receiver.GetStats();
	TestEqual(TEXT("FramesReceived should be 0 before start"), Stats.FramesReceived, static_cast<int64>(0));
	TestEqual(TEXT("BytesReceived should be 0 before start"), Stats.BytesReceived, static_cast<int64>(0));
	TestEqual(TEXT("DroppedFrames should be 0 before start"), Stats.DroppedFrames, static_cast<int64>(0));

	Receiver.Stop();
	return true;
}

// Test: Advanced params parsing
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverAdvancedParamsTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.AdvancedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverAdvancedParamsTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("session/track");
	Config.AdvancedParams.Add(TEXT("track_namespace"), TEXT("mocap/custom"));
	Config.AdvancedParams.Add(TEXT("track_name"), TEXT("customTrack"));

	TestTrue(TEXT("Initialize with advanced params should succeed"), Receiver.Initialize(Config));
	Receiver.Stop();
	return true;
}

// Test: SupportsAudio returns false (Phase 4 not implemented)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverSupportsAudioTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.SupportsAudio", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverSupportsAudioTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	// Audio is Phase 4, currently returns false
	TestFalse(TEXT("SupportsAudio should return false (Phase 4 not implemented)"), Receiver.SupportsAudio());
	return true;
}

// Test: Start before Initialize fails
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverStartBeforeInitializeTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.StartBeforeInitialize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverStartBeforeInitializeTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;

	AddExpectedError(TEXT("MoQ receiver Start called before Initialize"), EAutomationExpectedMessageFlags::Contains, 1);

	// Start without Initialize should fail
	TestFalse(TEXT("Start before Initialize should return false"), Receiver.Start());

	return true;
}

// Test: Poll before start returns 0
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverPollBeforeStartTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.PollBeforeStart", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverPollBeforeStartTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/poll");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config));

	// Poll before Start should return 0
	TestEqual(TEXT("Poll before Start should return 0"), Receiver.Poll(), 0);

	Receiver.Stop();
	return true;
}

// Test: SetConsumer works correctly
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverSetConsumerTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.SetConsumer", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverSetConsumerTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/consumer");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config));

	// Create and set consumer
	TSharedPtr<FMoQTestFrameConsumer> Consumer = MakeShared<FMoQTestFrameConsumer>();
	Receiver.SetConsumer(Consumer);

	// This should not crash
	Receiver.Stop();
	return true;
}

// Test: SetAudioSink does not crash (Phase 4 stub)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverSetAudioSinkTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.SetAudioSink", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverSetAudioSinkTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/audio");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config));

	// SetAudioSink with nullptr should not crash (Phase 4 stub)
	FO3DTransportAudioConfig AudioConfig;
	Receiver.SetAudioSink(nullptr, AudioConfig);

	Receiver.Stop();
	return true;
}

// Test: Alternative relay URL options
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverRelayUrlOptionsTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.RelayUrlOptions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverRelayUrlOptionsTest::RunTest(const FString& Parameters)
{
	// Test with relay_url advanced param
	{
		FO3DMoQReceiver Receiver;
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.StreamId = TEXT("test/relay");
		Config.AdvancedParams.Add(TEXT("relay_url"), TEXT("https://relay.example.com:4443"));

		TestTrue(TEXT("Initialize with relay_url param should succeed"), Receiver.Initialize(Config));
		Receiver.Stop();
	}

	// Test with moq.relay advanced param
	{
		FO3DMoQReceiver Receiver;
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.StreamId = TEXT("test/relay2");
		Config.AdvancedParams.Add(TEXT("moq.relay"), TEXT("https://relay2.example.com:4443"));

		TestTrue(TEXT("Initialize with moq.relay param should succeed"), Receiver.Initialize(Config));
		Receiver.Stop();
	}

	return true;
}

// Test: Stats reset on Initialize
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverStatsResetOnInitTest, "Open3DBroadcast.Open3DTransportMoQ.Receiver.StatsResetOnInit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverStatsResetOnInitTest::RunTest(const FString& Parameters)
{
	FO3DMoQReceiver Receiver;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/statsreset");

	TestTrue(TEXT("First Initialize should succeed"), Receiver.Initialize(Config));

	// Get initial stats
	FO3DTransportStats Stats1 = Receiver.GetStats();

	// Reinitialize
	TestTrue(TEXT("Second Initialize should succeed"), Receiver.Initialize(Config));

	// Stats should be reset
	FO3DTransportStats Stats2 = Receiver.GetStats();
	TestEqual(TEXT("Stats should be reset after reinit"), Stats2.FramesReceived, static_cast<int64>(0));

	Receiver.Stop();
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
