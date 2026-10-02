// Copyright (c) Open3DStream Contributors
//
// MoQ receiver unit tests. Every receiver is built on the fake moq-ffi table through
// Testing/MoQTesting.h, so no test loads moq_ffi.dll or touches the network (WP-T2).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeLock.h"

#include "Transport/O3DTransportTypes.h"
#include "Transport/O3DSerializedFrameConsumer.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "Testing/MoQTesting.h"
#include "Transport/MoQ/MoQFakeFfi.h"

// Mock consumer for testing
class FMoQTestFrameConsumer : public ISerializedFrameConsumer
{
public:
	virtual void SubmitFrame(const FString& Subject, TConstArrayView<uint8> Buffer, double TimestampSeconds) override
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverRequiresUriTest, "Open3DBroadcast.Transport.MoQ.Receiver.RequiresUri", O3DB_TEST_FLAGS)
bool FMoQReceiverRequiresUriTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");

	AddExpectedError(TEXT("MoQ receiver configuration invalid"), EAutomationExpectedMessageFlags::Contains, 1);

	TestFalse(TEXT("Initialize should fail when relay URI is missing"), Receiver.Initialize(Config).IsOk());
	return true;
}

// Test: Receiver initializes with valid config
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverInitializeSuccessTest, "Open3DBroadcast.Transport.MoQ.Receiver.Initialize", O3DB_TEST_FLAGS)
bool FMoQReceiverInitializeSuccessTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("session/testTrack");

	TestTrue(TEXT("Initialize should succeed with valid relay URI"), Receiver.Initialize(Config).IsOk());

	// Ensure Stop is safe to call immediately after initialization
	Receiver.Stop();
	return true;
}

// Test: Stop is idempotent
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverStopIsIdempotentTest, "Open3DBroadcast.Transport.MoQ.Receiver.StopIdempotent", O3DB_TEST_FLAGS)
bool FMoQReceiverStopIsIdempotentTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/idempotent");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	// Multiple Stop calls should not crash
	Receiver.Stop();
	Receiver.Stop();
	Receiver.Stop();

	return true;
}

// Test: Initialize can be called twice
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverInitializeCalledTwiceTest, "Open3DBroadcast.Transport.MoQ.Receiver.InitializeTwice", O3DB_TEST_FLAGS)
bool FMoQReceiverInitializeCalledTwiceTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/reinit");

	TestTrue(TEXT("First Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	// Calling Initialize again should succeed (replaces config)
	Config.StreamId = TEXT("test/reinit2");
	TestTrue(TEXT("Second Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	Receiver.Stop();
	return true;
}

// Test: GetStats before start
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverGetStatsBeforeStartTest, "Open3DBroadcast.Transport.MoQ.Receiver.GetStatsBeforeStart", O3DB_TEST_FLAGS)
bool FMoQReceiverGetStatsBeforeStartTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/stats");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	const FO3DTransportStats Stats = Receiver.GetStats();
	TestEqual(TEXT("FramesReceived should be 0 before start"), Stats.FramesReceived, static_cast<int64>(0));
	TestEqual(TEXT("BytesReceived should be 0 before start"), Stats.BytesReceived, static_cast<int64>(0));
	TestEqual(TEXT("DroppedFrames should be 0 before start"), Stats.DroppedFrames, static_cast<int64>(0));

	Receiver.Stop();
	return true;
}

// Test: Advanced params parsing
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverAdvancedParamsTest, "Open3DBroadcast.Transport.MoQ.Receiver.AdvancedParams", O3DB_TEST_FLAGS)
bool FMoQReceiverAdvancedParamsTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("session/track");
	Config.AdvancedParams.Add(TEXT("track_namespace"), TEXT("mocap/custom"));
	Config.AdvancedParams.Add(TEXT("track_name"), TEXT("customTrack"));

	TestTrue(TEXT("Initialize with advanced params should succeed"), Receiver.Initialize(Config).IsOk());
	Receiver.Stop();
	return true;
}

// MoQ audio shipped (Phase 4, WP-S5/S8); this used to assert the old "not implemented" stub.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverSupportsAudioTest, "Open3DBroadcast.Transport.MoQ.Receiver.SupportsAudio", O3DB_TEST_FLAGS)
bool FMoQReceiverSupportsAudioTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> Receiver = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	TestTrue(TEXT("The MoQ receiver advertises audio"), Receiver->SupportsAudio());
	return true;
}

// Test: Start before Initialize fails
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverStartBeforeInitializeTest, "Open3DBroadcast.Transport.MoQ.Receiver.StartBeforeInitialize", O3DB_TEST_FLAGS)
bool FMoQReceiverStartBeforeInitializeTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;

	AddExpectedError(TEXT("MoQ receiver Start called before Initialize"), EAutomationExpectedMessageFlags::Contains, 1);

	// Start without Initialize should fail
	TestFalse(TEXT("Start before Initialize should return false"), Receiver.Start().IsOk());

	return true;
}

// Test: Poll before start returns 0
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverPollBeforeStartTest, "Open3DBroadcast.Transport.MoQ.Receiver.PollBeforeStart", O3DB_TEST_FLAGS)
bool FMoQReceiverPollBeforeStartTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/poll");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	// Poll before Start should return 0
	TestEqual(TEXT("Poll before Start should return 0"), Receiver.Poll(), 0);

	Receiver.Stop();
	return true;
}

// Test: SetConsumer works correctly
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverSetConsumerTest, "Open3DBroadcast.Transport.MoQ.Receiver.SetConsumer", O3DB_TEST_FLAGS)
bool FMoQReceiverSetConsumerTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/consumer");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	// Create and set consumer
	TSharedPtr<FMoQTestFrameConsumer> Consumer = MakeShared<FMoQTestFrameConsumer>();
	Receiver.SetConsumer(Consumer);

	// This should not crash
	Receiver.Stop();
	return true;
}

// Test: clearing the audio sink with nullptr is safe
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverSetAudioSinkTest, "Open3DBroadcast.Transport.MoQ.Receiver.SetAudioSink", O3DB_TEST_FLAGS)
bool FMoQReceiverSetAudioSinkTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/audio");

	TestTrue(TEXT("Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	// SetAudioSink with nullptr disables audio delivery and must not crash
	FO3DTransportAudioConfig AudioConfig;
	Receiver.SetAudioSink(nullptr, AudioConfig);

	Receiver.Stop();
	return true;
}

// Test: Alternative relay URL options
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverRelayUrlOptionsTest, "Open3DBroadcast.Transport.MoQ.Receiver.RelayUrlOptions", O3DB_TEST_FLAGS)
bool FMoQReceiverRelayUrlOptionsTest::RunTest(const FString& Parameters)
{
	// Test with relay_url advanced param
	{
		const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
		const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
		IOpen3DReceiver& Receiver = *ReceiverRef;
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.StreamId = TEXT("test/relay");
		Config.AdvancedParams.Add(TEXT("relay_url"), TEXT("https://relay.example.com:4443"));

		TestTrue(TEXT("Initialize with relay_url param should succeed"), Receiver.Initialize(Config).IsOk());
		Receiver.Stop();
	}

	// Test with moq.relay advanced param
	{
		const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
		const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
		IOpen3DReceiver& Receiver = *ReceiverRef;
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.StreamId = TEXT("test/relay2");
		Config.AdvancedParams.Add(TEXT("moq.relay"), TEXT("https://relay2.example.com:4443"));

		TestTrue(TEXT("Initialize with moq.relay param should succeed"), Receiver.Initialize(Config).IsOk());
		Receiver.Stop();
	}

	return true;
}

// Test: Stats reset on Initialize
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverStatsResetOnInitTest, "Open3DBroadcast.Transport.MoQ.Receiver.StatsResetOnInit", O3DB_TEST_FLAGS)
bool FMoQReceiverStatsResetOnInitTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DReceiver> ReceiverRef = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DReceiver& Receiver = *ReceiverRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/statsreset");

	TestTrue(TEXT("First Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	// Get initial stats
	FO3DTransportStats Stats1 = Receiver.GetStats();

	// Reinitialize
	TestTrue(TEXT("Second Initialize should succeed"), Receiver.Initialize(Config).IsOk());

	// Stats should be reset
	FO3DTransportStats Stats2 = Receiver.GetStats();
	TestEqual(TEXT("Stats should be reset after reinit"), Stats2.FramesReceived, static_cast<int64>(0));

	Receiver.Stop();
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
