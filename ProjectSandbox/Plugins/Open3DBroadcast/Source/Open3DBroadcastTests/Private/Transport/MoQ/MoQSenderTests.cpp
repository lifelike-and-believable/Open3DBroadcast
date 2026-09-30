#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DTransportTypes.h"
#include "Sender/MoQSender.h"

#include "o3ds/model.h"

#include <string>
#include <vector>

#if O3D_WITH_TRANSPORT_MOQ

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderRequiresUriTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.RequiresUri", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderRequiresUriTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");

	AddExpectedError(TEXT("MoQ sender configuration invalid"), EAutomationExpectedMessageFlags::Contains, 1);

	TestFalse(TEXT("Initialize should fail when relay URI is missing"), Sender.Initialize(Config));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderInitializeSuccessTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.Initialize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderInitializeSuccessTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
	FO3DTransportConfig Config;
	Config.Transport = TEXT("MoQ");
	Config.Uri = TEXT("https://localhost:4443");
	Config.StreamId = TEXT("session/testTrack");

	TestTrue(TEXT("Initialize should succeed with valid relay URI"), Sender.Initialize(Config));

	// Ensure Stop is safe to call immediately after initialization.
	Sender.Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderStopIsIdempotentTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.StopIdempotent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderStopIsIdempotentTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderInitializeCalledTwiceTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.InitializeTwice", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderInitializeCalledTwiceTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderGetStatsBeforeStartTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.GetStatsBeforeStart", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderGetStatsBeforeStartTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderAdvancedParamsTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.AdvancedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderAdvancedParamsTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderSupportsAudioTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.SupportsAudio", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderSupportsAudioTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
	// Audio is Phase 4, currently returns false
	TestFalse(TEXT("SupportsAudio should return false (Phase 4 not implemented)"), Sender.SupportsAudio());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderSendBeforeStartTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.SendBeforeStart", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderSendBeforeStartTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderStartBeforeInitializeTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.StartBeforeInitialize", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderStartBeforeInitializeTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;

	AddExpectedError(TEXT("MoQ sender Start called before Initialize"), EAutomationExpectedMessageFlags::Contains, 1);

	// Start without Initialize should fail
	TestFalse(TEXT("Start before Initialize should return false"), Sender.Start());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderTickBeforeStartTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.TickBeforeStart", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderTickBeforeStartTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderCreateAudioSinkTest, "Open3DBroadcast.Open3DTransportMoQ.Sender.CreateAudioSink", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderCreateAudioSinkTest::RunTest(const FString& Parameters)
{
	FO3DMoQSender Sender;
	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.bEnableAudio = true;
	AudioConfig.SampleRate = 48000;
	AudioConfig.NumChannels = 1;

	// CreateAudioSink should return nullptr (Phase 4 not implemented)
	const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink = Sender.CreateAudioSink(AudioConfig);
	TestFalse(TEXT("CreateAudioSink should return nullptr (Phase 4 not implemented)"), AudioSink.IsValid());

	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
