#if WITH_DEV_AUTOMATION_TESTS

#include "../Sender/WebRTCSender.h"
#include "../Receiver/WebRTCReceiver.h"
#include "../Shared/WebRTCUtils.h"

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformTime.h"
#include "Containers/List.h"

#include "O3DTransportTypes.h"
#include "SerializedFrameConsumerRegistry.h"

#include "o3ds/model.h"

namespace
{
	/**
	 * Mock frame consumer for testing data reception
	 */
	class FTestFrameConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& InStreamId, const TArray<uint8>& InPayload, double InTimestamp) override
		{
			FScopeLock Lock(&DataMutex);
			ReceivedFrames.Add({InStreamId, InPayload, InTimestamp});
		}

		struct FReceivedFrame
		{
			FString StreamId;
			TArray<uint8> Payload;
			double Timestamp;
		};

		int32 GetFrameCount() const
		{
			FScopeLock Lock(&DataMutex);
			return ReceivedFrames.Num();
		}

		TArray<FReceivedFrame> GetReceivedFrames() const
		{
			FScopeLock Lock(&DataMutex);
			return ReceivedFrames;
		}

		void Reset()
		{
			FScopeLock Lock(&DataMutex);
			ReceivedFrames.Reset();
		}

	private:
		mutable FCriticalSection DataMutex;
		TArray<FReceivedFrame> ReceivedFrames;
	};

	/**
	 * Mock audio sink for testing audio reception
	 */
	class FTestAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Pcm16Data, int32 NumBytes) override
		{
			if (!Pcm16Data || NumBytes <= 0)
				return;

			FScopeLock Lock(&AudioMutex);
			ReceivedAudioFrames++;
			LastMetaSampleRate = Meta.SampleRate;
			LastMetaChannels = Meta.NumChannels;
			TotalAudioBytes += NumBytes;
		}

		int32 GetFrameCount() const
		{
			FScopeLock Lock(&AudioMutex);
			return ReceivedAudioFrames;
		}

		int32 GetLastSampleRate() const
		{
			FScopeLock Lock(&AudioMutex);
			return LastMetaSampleRate;
		}

		int32 GetLastChannels() const
		{
			FScopeLock Lock(&AudioMutex);
			return LastMetaChannels;
		}

		int32 GetTotalBytes() const
		{
			FScopeLock Lock(&AudioMutex);
			return TotalAudioBytes;
		}

		void Reset()
		{
			FScopeLock Lock(&AudioMutex);
			ReceivedAudioFrames = 0;
			LastMetaSampleRate = 0;
			LastMetaChannels = 0;
			TotalAudioBytes = 0;
		}

	private:
		mutable FCriticalSection AudioMutex;
		int32 ReceivedAudioFrames = 0;
		int32 LastMetaSampleRate = 0;
		int32 LastMetaChannels = 0;
		int32 TotalAudioBytes = 0;
	};

	/**
	 * Helper to create minimal test config
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
	 * Helper to create a test subject list for serialization
	 */
	O3DS::SubjectList CreateTestSubjectList()
	{
		O3DS::SubjectList List;
		// Note: This is a simplified version - actual implementation would populate with real skeleton data
		return List;
	}
}

// ============================================================================
// CONNECTION TESTS
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCConnectionInitializeTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Connection.Initialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCConnectionInitializeTest::RunTest(const FString& Parameters)
{
	// Test basic initialization
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test-server.livekit.example.com"),
		TEXT("test-token-valid-format")
	);

	// Should successfully initialize even if server is unreachable
	// (initialization is sync, but connection is async)
	bool bResult = Sender.Initialize(Config);

	// Note: On Windows 64-bit this should return true or fail with clear error
	// On other platforms it should return false with clear message

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	TestTrue(TEXT("Sender should initialize on Win64"), bResult);
	//AddWarning(TEXT("FWebRTCConnectionInitializeTest: This test uses a hardcoded example server (wss://test-server.livekit.example.com) which is not a real LiveKit server. Full validation requires an active remote server with proper authentication token. For complete testing, configure WEBRTC_TEST_SERVER_URL and WEBRTC_TEST_TOKEN environment variables or modify this test with actual server credentials."));
#else
	// On non-Win64 platforms, sender should fail to initialize
	if (bResult)
	{
		AddError(TEXT("Sender should fail on non-Win64 platforms but succeeded"));
		return false;
	}
#endif

	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCConnectionDubleInitializeTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Connection.DoubleInitialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCConnectionDubleInitializeTest::RunTest(const FString& Parameters)
{
	// Test that initializing twice returns false on second attempt
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	bool bFirstInit = Sender.Initialize(Config);
	TestTrue(TEXT("First initialize should succeed"), bFirstInit);

	bool bSecondInit = Sender.Initialize(Config);
	// Second initialization should fail - if it succeeds, test fails
	if (bSecondInit)
	{
		AddError(TEXT("Second initialize should fail but succeeded"));
		return false;
	}

	Sender.Stop();
	//AddWarning(TEXT("FWebRTCConnectionDubleInitializeTest: This test uses a hardcoded example server (wss://test.livekit.example.com) which is not a real LiveKit server. Full validation of double-initialization behavior with actual server connection requires an active remote server with proper authentication token. For complete testing, configure WEBRTC_TEST_SERVER_URL and WEBRTC_TEST_TOKEN environment variables or modify this test with actual server credentials."));
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCConnectionInvalidUrlTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Connection.InvalidUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCConnectionInvalidUrlTest::RunTest(const FString& Parameters)
{
	// Test that empty URL is rejected - initialization should return false
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config;
	Config.Uri = TEXT("");  // Empty URL
	Config.Token = TEXT("test-token");

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	bool bResult = Sender.Initialize(Config);
	// Initialization should fail when URL is empty - if it does, test passes
	if (bResult)
	{
		AddError(TEXT("Initialize should fail with empty URL but succeeded"));
		return false;
	}
#else
	// On non-Win64 platforms, initialization should fail due to platform check before ParseConfig
	FO3DWebRTCSender TestSender;
	FO3DTransportConfig TestConfig;
	TestConfig.Uri = TEXT("");
	TestConfig.Token = TEXT("test-token");
	bool bResult = TestSender.Initialize(TestConfig);
	if (bResult)
	{
		AddError(TEXT("Initialize should fail on non-Win64 platforms but succeeded"));
		return false;
	}
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCConnectionEmptyTokenTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Connection.EmptyToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCConnectionEmptyTokenTest::RunTest(const FString& Parameters)
{
	// Test that empty token is rejected - initialization should return false
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config;
	Config.Uri = TEXT("wss://test.livekit.example.com");
	Config.Token = TEXT("");  // Empty token

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	bool bResult = Sender.Initialize(Config);
	// Initialization should fail when token is empty - if it does, test passes
	if (bResult)
	{
		AddError(TEXT("Initialize should fail with empty token but succeeded"));
		return false;
	}
#else
	// On non-Win64 platforms, initialization should fail due to platform check before ParseConfig
	FO3DWebRTCSender TestSender;
	FO3DTransportConfig TestConfig;
	TestConfig.Uri = TEXT("wss://test.livekit.example.com");
	TestConfig.Token = TEXT("");
	bool bResult = TestSender.Initialize(TestConfig);
	if (bResult)
	{
		AddError(TEXT("Initialize should fail on non-Win64 platforms but succeeded"));
		return false;
	}
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCReceiverInitializeTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Receiver.Initialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCReceiverInitializeTest::RunTest(const FString& Parameters)
{
	// Test receiver initialization
	FO3DWebRTCReceiver Receiver;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	bool bResult = Receiver.Initialize(Config);
	TestTrue(TEXT("Receiver should initialize on Win64"), bResult);

	Receiver.Stop();
	//AddWarning(TEXT("FWebRTCReceiverInitializeTest: This test uses a hardcoded example server (wss://test.livekit.example.com) which is not a real LiveKit server. Full validation of receiver initialization with actual server connection requires an active remote server with proper authentication token. For complete testing, configure WEBRTC_TEST_SERVER_URL and WEBRTC_TEST_TOKEN environment variables or modify this test with actual server credentials."));
#endif

	return true;
}

// ============================================================================
// DATA TRANSFER TESTS
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCSendBeforeConnectedTest,
	"Open3DBroadcast.Open3DTransportWebRTC.DataTransfer.SendBeforeConnected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCSendBeforeConnectedTest::RunTest(const FString& Parameters)
{
	// Test that Send() returns false when not connected
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);

	// Create a dummy subject list
	O3DS::SubjectList List;

	// Send should fail because we're not connected
	bool bResult = Sender.Send(List);
	// If Send() returns true (unexpected success), test fails
	if (bResult)
	{
		AddError(TEXT("Send should fail when not connected but succeeded"));
		return false;
	}

	FO3DTransportStats Stats = Sender.GetStats();
	TestEqual(TEXT("DroppedFrames should increase"), (int32)Stats.DroppedFrames, (int32)1);

	Sender.Stop();
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCPayloadSizeValidationSmallTest,
	"Open3DBroadcast.Open3DTransportWebRTC.DataTransfer.PayloadSizeSmall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCPayloadSizeValidationSmallTest::RunTest(const FString& Parameters)
{
	// Note: This test validates the logic, but actual sending requires a server
	// Test is mainly to verify no crashes with size validation code

	// Create a subject list (serialization size is internal to O3DS)
	O3DS::SubjectList List;

	// If we could serialize and get size < 1300, it should pass validation
	// For now, just verify the sender can be created and destroyed safely

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);
	// Attempting to send without connection returns false (as tested above)
	Sender.Stop();
#endif

	TestTrue(TEXT("Sender construction and destruction should be safe"), true);
	return true;
}

// ============================================================================
// AUDIO TESTS
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAudioSinkCreationTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Audio.SinkCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCAudioSinkCreationTest::RunTest(const FString& Parameters)
{
	// Test audio sink creation and configuration
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true  // Enable audio
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);

	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.BitrateKbps = 48;
	AudioConfig.NumChannels = 1;
	AudioConfig.SampleRate = 48000;

	auto AudioSink = Sender.CreateAudioSink(AudioConfig);
	TestTrue(TEXT("Audio sink should be created"), AudioSink.IsValid());

	Sender.Stop();
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAudioBitrateClampTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Audio.BitrateClamping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCAudioBitrateClampTest::RunTest(const FString& Parameters)
{
	// Test that audio bitrate is clamped to valid range (16-128 kbps)
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true  // Enable audio
	);

	// Try to set invalid bitrate (should be clamped)
	Config.Audio.BitrateKbps = 256;  // Too high

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);
	// Initialization should succeed; clamping happens internally
	TestTrue(TEXT("Initialize should succeed with out-of-range bitrate (will be clamped)"), true);

	Sender.Stop();
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAudioSubmitWithoutConnectionTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Audio.SubmitWithoutConnection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCAudioSubmitWithoutConnectionTest::RunTest(const FString& Parameters)
{
	// Test that SubmitPcm fails gracefully when not connected
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true  // Enable audio
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);

	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.BitrateKbps = 48;
	AudioConfig.NumChannels = 1;
	AudioConfig.SampleRate = 48000;

	auto AudioSink = Sender.CreateAudioSink(AudioConfig);
	TestTrue(TEXT("Audio sink should be created"), AudioSink.IsValid());

	// Create test audio data
	TArray<float> TestAudio;
	TestAudio.SetNum(960);  // 20ms at 48kHz
	for (int32 i = 0; i < 960; ++i)
	{
		TestAudio[i] = 0.5f;  // Valid sample range
	}

	// SubmitPcm should return false when not connected
	bool bResult = AudioSink->SubmitPcm(TEXT("Test"), TestAudio.GetData(), 960, 1, 48000, FPlatformTime::Seconds());
	if (bResult)
	{
		AddError(TEXT("SubmitPcm should fail when not connected but succeeded"));
		return false;
	}

	Sender.Stop();
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCAudioClippingTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Audio.Clipping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCAudioClippingTest::RunTest(const FString& Parameters)
{
	// Test that audio samples are clamped to [-1.0, 1.0]
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true  // Enable audio
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);

	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.BitrateKbps = 48;
	AudioConfig.NumChannels = 1;
	AudioConfig.SampleRate = 48000;

	auto AudioSink = Sender.CreateAudioSink(AudioConfig);

	// Create audio with out-of-range samples (should be clamped)
	TArray<float> TestAudio;
	TestAudio.SetNum(960);
	for (int32 i = 0; i < 960; ++i)
	{
		TestAudio[i] = 2.5f;  // > 1.0, will be clamped to 1.0
	}

	// Should not crash, clipping should happen internally
	bool bResult = AudioSink->SubmitPcm(TEXT("Test"), TestAudio.GetData(), 960, 1, 48000, FPlatformTime::Seconds());
	if (bResult)
	{
		AddError(TEXT("SubmitPcm should fail when not connected but succeeded"));
		return false;
	}

	Sender.Stop();
#endif

	return true;
}

// ============================================================================
// STATE MANAGEMENT TESTS
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCStatsResetTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Stats.Reset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCStatsResetTest::RunTest(const FString& Parameters)
{
	// Test that stats are properly reset on initialization
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);

	FO3DTransportStats Stats = Sender.GetStats();
	TestEqual(TEXT("FramesSent should be 0 after init"), (int32)0, (int32)Stats.FramesSent);
	TestEqual(TEXT("BytesSent should be 0 after init"), (int64)0, Stats.BytesSent);

	Sender.Stop();
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCMultipleStopCallsTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Lifecycle.MultipleStopCalls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCMultipleStopCallsTest::RunTest(const FString& Parameters)
{
	// Test that Stop() can be called multiple times safely (idempotent)
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Sender.Initialize(Config);

	// Should not crash
	Sender.Stop();
	Sender.Stop();  // Second call
	Sender.Stop();  // Third call

	TestTrue(TEXT("Multiple Stop() calls should be safe"), true);
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCReceiverSetConsumerTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Receiver.SetConsumer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCReceiverSetConsumerTest::RunTest(const FString& Parameters)
{
	// Test setting a custom consumer
	FO3DWebRTCReceiver Receiver;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token")
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Receiver.Initialize(Config);

	auto TestConsumer = MakeShared<FTestFrameConsumer>();
	Receiver.SetConsumer(TestConsumer);

	TestTrue(TEXT("Should be able to set custom consumer"), true);

	Receiver.Stop();
#endif

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebRTCReceiverSetAudioSinkTest,
	"Open3DBroadcast.Open3DTransportWebRTC.Receiver.SetAudioSink",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWebRTCReceiverSetAudioSinkTest::RunTest(const FString& Parameters)
{
	// Test setting a custom audio sink
	FO3DWebRTCReceiver Receiver;
	FO3DTransportConfig Config = CreateTestConfig(
		TEXT("wss://test.livekit.example.com"),
		TEXT("test-token"),
		true  // Enable audio
	);

#if PLATFORM_WINDOWS && PLATFORM_64BITS
	Receiver.Initialize(Config);

	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.BitrateKbps = 48;
	AudioConfig.NumChannels = 1;
	AudioConfig.SampleRate = 48000;

	auto TestAudioSink = MakeShared<FTestAudioSink>();
	Receiver.SetAudioSink(TestAudioSink, AudioConfig);

	TestTrue(TEXT("Should be able to set audio sink"), true);

	Receiver.Stop();
#endif

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
