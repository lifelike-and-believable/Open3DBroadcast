// Copyright Lifelike & Believable. All Rights Reserved.

//
// ADR 0011 item 10 (CTL-2): the receiver source rejects bytes it cannot read (for example
// control messages reaching a mocap-only path) with one throttled warning, not one per packet,
// and still counts every rejection.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DPerformanceMetrics.h"
#include "O3DUnifiedMessage.h"
#include "Testing/O3DReceiverTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverMalformedWarningThrottleTest, "Open3DBroadcast.Receiver.MalformedPacketWarningIsThrottled", O3DB_TEST_FLAGS)
bool FO3DReceiverMalformedWarningThrottleTest::RunTest(const FString& Parameters)
{
	using FAccessor = FO3DReceiverCorrectnessTestAccessor;

	FO3DReceiverSourceConfig Config;
	Config.TransportName = FName(TEXT("loopback")); // never started; frames come from the test
	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Config);
	const TSharedRef<FAccessor::FRecorder> Recorder = MakeShared<FAccessor::FRecorder>();
	FAccessor::BindRecorder(*Source, Recorder);
	const TSharedRef<ISerializedFrameConsumer> Consumer = FAccessor::MakeConsumer(Source);

	// Exactly one warning for a flood within the throttle interval (10 s; this loop takes far less).
	AddExpectedMessage(TEXT("Rejected malformed packet"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	// What an old receiver's mocap path sees when a sender emits control: an envelope.
	const TArray<uint8> Payload = { 1, 2, 3, 4, 5, 6, 7, 8 };
	TArray<uint8> ControlEnvelope;
	O3DS::WriteControlEnvelope(Payload, 1.0, ControlEnvelope);

	const uint64 ErrorsBefore = FO3DPerformanceMetrics::Get().GetReceiverMetrics().DeserializationErrors.load();
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		Consumer->SubmitFrame(TEXT("__o3d.ctl"), ControlEnvelope, 0.0);
	}
	const uint64 ErrorsAfter = FO3DPerformanceMetrics::Get().GetReceiverMetrics().DeserializationErrors.load();

	TestEqual(TEXT("Every rejection still counted"), ErrorsAfter - ErrorsBefore, static_cast<uint64>(1000));
	TestEqual(TEXT("Nothing reached LiveLink"), Recorder->Frames.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
