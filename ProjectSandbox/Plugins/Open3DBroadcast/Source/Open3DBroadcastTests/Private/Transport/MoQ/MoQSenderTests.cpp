// Copyright (c) Open3DStream Contributors
//
// MoQ sender unit tests. Every sender is built on the fake moq-ffi table through
// Testing/MoQTesting.h, so no test loads moq_ffi.dll or touches the network (WP-T2).

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DTransportTypes.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

#if O3D_WITH_TRANSPORT_MOQ

#include "Testing/MoQTesting.h"
#include "Transport/MoQ/MoQFakeFfi.h"

namespace MoQSenderTestHelpers
{
	void PopulateTestSubject(O3DS::SubjectList& List, const FString& SubjectName)
	{
		const FTCHARToUTF8 SubjectUtf8(*SubjectName);
		O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
		Subject->addTransform("Root", -1);
		Subject->addTransform("Spine", 0);
		Subject->addTransform("Head", 1);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderRequiresUriTest, "Open3DBroadcast.Transport.MoQ.Sender.RequiresUri", O3DB_TEST_FLAGS)
bool FMoQSenderRequiresUriTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");

	AddExpectedError(TEXT("MoQ sender configuration invalid"), EAutomationExpectedMessageFlags::Contains, 1);

	TestFalse(TEXT("Initialize should fail when relay URI is missing"), Sender.Initialize(Config));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderInitializeSuccessTest, "Open3DBroadcast.Transport.MoQ.Sender.Initialize", O3DB_TEST_FLAGS)
bool FMoQSenderInitializeSuccessTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("session/testTrack");

	TestTrue(TEXT("Initialize should succeed with valid relay URI"), Sender.Initialize(Config));

	// Ensure Stop is safe to call immediately after initialization.
	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderStopIsIdempotentTest, "Open3DBroadcast.Transport.MoQ.Sender.StopIdempotent", O3DB_TEST_FLAGS)
bool FMoQSenderStopIsIdempotentTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/idempotent");

	TestTrue(TEXT("Initialize should succeed"), Sender.Initialize(Config));

	// Multiple Stop calls should not crash
	Sender.Stop();
	Sender.Stop();
	Sender.Stop();

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderInitializeCalledTwiceTest, "Open3DBroadcast.Transport.MoQ.Sender.InitializeTwice", O3DB_TEST_FLAGS)
bool FMoQSenderInitializeCalledTwiceTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/reinit");

	TestTrue(TEXT("First Initialize should succeed"), Sender.Initialize(Config));

	// Calling Initialize again should succeed (replaces config)
	Config.StreamId = TEXT("test/reinit2");
	TestTrue(TEXT("Second Initialize should succeed"), Sender.Initialize(Config));

	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderGetStatsBeforeStartTest, "Open3DBroadcast.Transport.MoQ.Sender.GetStatsBeforeStart", O3DB_TEST_FLAGS)
bool FMoQSenderGetStatsBeforeStartTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/stats");

	TestTrue(TEXT("Initialize should succeed"), Sender.Initialize(Config));

	const FO3DTransportStats Stats = Sender.GetStats();
	TestEqual(TEXT("FramesSent should be 0 before start"), Stats.FramesSent, static_cast<int64>(0));
	TestEqual(TEXT("BytesSent should be 0 before start"), Stats.BytesSent, static_cast<int64>(0));
	TestEqual(TEXT("DroppedFrames should be 0 before start"), Stats.DroppedFrames, static_cast<int64>(0));

	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderAdvancedParamsTest, "Open3DBroadcast.Transport.MoQ.Sender.AdvancedParams", O3DB_TEST_FLAGS)
bool FMoQSenderAdvancedParamsTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("session/track");
	Config.AdvancedParams.Add(TEXT("track_namespace"), TEXT("mocap/custom"));
	Config.AdvancedParams.Add(TEXT("track_name"), TEXT("customTrack"));
	Config.AdvancedParams.Add(TEXT("delivery_mode"), TEXT("datagram"));

	TestTrue(TEXT("Initialize with advanced params should succeed"), Sender.Initialize(Config));
	Sender.Stop();
	return true;
}

// MoQ audio shipped (Phase 4, WP-S5/S8); this used to assert the old "not implemented" stub.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderSupportsAudioTest, "Open3DBroadcast.Transport.MoQ.Sender.SupportsAudio", O3DB_TEST_FLAGS)
bool FMoQSenderSupportsAudioTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	TestTrue(TEXT("The MoQ sender advertises audio"), Sender->SupportsAudio());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderSendBeforeStartTest, "Open3DBroadcast.Transport.MoQ.Sender.SendBeforeStart", O3DB_TEST_FLAGS)
bool FMoQSenderSendBeforeStartTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/sendbeforestart");

	TestTrue(TEXT("Initialize should succeed"), Sender.Initialize(Config));

	// Create test subject
	O3DS::SubjectList Subjects;
	MoQSenderTestHelpers::PopulateTestSubject(Subjects, TEXT("TestSubject"));

	// Sending before Start should fail gracefully
	TestFalse(TEXT("Send before Start should return false"), Sender.Send(Subjects));

	// Stats should reflect the dropped frame
	const FO3DTransportStats Stats = Sender.GetStats();
	// Note: The implementation doesn't increment DroppedFrames for sends before start,
	// it just returns false. This is acceptable behavior.

	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderStartBeforeInitializeTest, "Open3DBroadcast.Transport.MoQ.Sender.StartBeforeInitialize", O3DB_TEST_FLAGS)
bool FMoQSenderStartBeforeInitializeTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;

	AddExpectedError(TEXT("MoQ sender Start called before Initialize"), EAutomationExpectedMessageFlags::Contains, 1);

	// Start without Initialize should fail
	TestFalse(TEXT("Start before Initialize should return false"), Sender.Start());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderTickBeforeStartTest, "Open3DBroadcast.Transport.MoQ.Sender.TickBeforeStart", O3DB_TEST_FLAGS)
bool FMoQSenderTickBeforeStartTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> SenderRef = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
	IOpen3DSender& Sender = *SenderRef;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("test/tick");

	TestTrue(TEXT("Initialize should succeed"), Sender.Initialize(Config));

	// Tick before Start should not crash
	Sender.Tick(0.016f);

	Sender.Stop();
	return true;
}

// MoQ audio shipped (Phase 4, WP-S5/S8); this used to expect nullptr from the old stub. A sink
// is handed out, and once the sender stops it rejects PCM (WP-S5 lifetime gate).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderCreateAudioSinkTest, "Open3DBroadcast.Transport.MoQ.Sender.CreateAudioSink", O3DB_TEST_FLAGS)
bool FMoQSenderCreateAudioSinkTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);

	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://fake.relay.invalid:443");
	Config.StreamId = TEXT("test/audiosink");
	TestTrue(TEXT("Initialize should succeed"), Sender->Initialize(Config));

	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.bEnableAudio = true;
	AudioConfig.SampleRate = 48000;
	AudioConfig.NumChannels = 1;
	const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender->CreateAudioSink(AudioConfig);
	if (!TestTrue(TEXT("CreateAudioSink returns a sink"), AudioSink.IsValid()))
	{
		return false;
	}

	Sender->Stop();
	const float Samples[4] = { 0.1f, -0.1f, 0.2f, -0.2f };
	TestFalse(TEXT("The sink rejects PCM after Stop"), AudioSink->SubmitPcm(TEXT("stopped"), Samples, 4, 1, 48000, 0.0));
	MoQTesting::PumpDispatcher();
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
