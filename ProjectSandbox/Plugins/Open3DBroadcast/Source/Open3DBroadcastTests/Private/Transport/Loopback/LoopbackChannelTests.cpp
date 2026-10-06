// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U6 (TRB-31, TRB-32): the loopback channel's limits belong to the sender, whatever order the
// two ends start in, and a stopped receiver delivers nothing.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DLoopbackChannelTests
{
	FO3DTransportConfig MakeConfig(const FString& Channel, EO3DTransportRole Role)
	{
		FO3DTransportConfig Config(TEXT("Loopback"), Role);
		Config.StreamId = Channel;
		Config.AdvancedParams.Add(TEXT("channel"), Channel);
		return Config;
	}

	EO3DSendResult SendFrame(IOpen3DSender& Sender, const TArray<uint8>& Frame)
	{
		return Sender.SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("LoopbackChannelTest"), 0.0));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackReceiverKeepsSenderLimitsTest, "Open3DBroadcast.Transport.Loopback.Channel.ReceiverKeepsSenderQueueCapacity", O3DB_TEST_FLAGS)
bool FO3DLoopbackReceiverKeepsSenderLimitsTest::RunTest(const FString& Parameters)
{
	using namespace O3DLoopbackChannelTests;
	AddExpectedError(TEXT("Loopback queue full"), EAutomationExpectedMessageFlags::Contains, 1);
	const FString Channel = O3DTests::MakeUniqueName(TEXT("o3dloopbacklimits"));
	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("LoopbackLimits"), 1);
	if (!TestEqual(TEXT("Frame built"), Frames.Num(), 1))
	{
		return false;
	}

	FO3DTransportConfig SenderConfig = MakeConfig(Channel, EO3DTransportRole::Sender);
	SenderConfig.AdvancedParams.Add(TEXT("loopback.maxqueue"), TEXT("1"));
	const TSharedPtr<IOpen3DSender> Sender = FO3DTransportRegistry::Get().CreateSender(TEXT("Loopback"));
	if (!TestTrue(TEXT("Sender created"), Sender.IsValid())
		|| !TestTrue(TEXT("Sender initializes"), Sender->Initialize(SenderConfig).IsOk())
		|| !TestTrue(TEXT("Sender starts"), Sender->Start().IsOk()))
	{
		return false;
	}

	// The receiver's config has no queue option, as the receiver's options panel never shows one.
	const TSharedPtr<IOpen3DReceiver> Receiver = FO3DTransportRegistry::Get().CreateReceiver(TEXT("Loopback"));
	if (!TestTrue(TEXT("Receiver created"), Receiver.IsValid())
		|| !TestTrue(TEXT("Receiver initializes after the sender"), Receiver->Initialize(MakeConfig(Channel, EO3DTransportRole::Receiver)).IsOk()))
	{
		Sender->Stop();
		return false;
	}

	TestTrue(TEXT("The first frame fills the sender's one-frame queue"), SendFrame(*Sender, Frames[0]) == EO3DSendResult::Queued);
	TestTrue(TEXT("The second frame is refused: the receiver left the sender's capacity alone"), SendFrame(*Sender, Frames[0]) == EO3DSendResult::DroppedBackpressure);

	Receiver->Stop();
	Sender->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DLoopbackStoppedReceiverPollTest, "Open3DBroadcast.Transport.Loopback.Receiver.PollAfterStopDeliversNothing", O3DB_TEST_FLAGS)
bool FO3DLoopbackStoppedReceiverPollTest::RunTest(const FString& Parameters)
{
	using namespace O3DLoopbackChannelTests;
	const FString Channel = O3DTests::MakeUniqueName(TEXT("o3dloopbackstop"));
	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("LoopbackStop"), 2);
	if (!TestEqual(TEXT("Frames built"), Frames.Num(), 2))
	{
		return false;
	}

	const TSharedPtr<IOpen3DSender> Sender = FO3DTransportRegistry::Get().CreateSender(TEXT("Loopback"));
	const TSharedPtr<IOpen3DReceiver> Receiver = FO3DTransportRegistry::Get().CreateReceiver(TEXT("Loopback"));
	const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
	if (!TestTrue(TEXT("Sender and receiver created"), Sender.IsValid() && Receiver.IsValid())
		|| !TestTrue(TEXT("Sender initializes"), Sender->Initialize(MakeConfig(Channel, EO3DTransportRole::Sender)).IsOk())
		|| !TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(MakeConfig(Channel, EO3DTransportRole::Receiver)).IsOk()))
	{
		return false;
	}
	Receiver->SetConsumer(Consumer);
	if (!TestTrue(TEXT("Sender starts"), Sender->Start().IsOk()) || !TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk()))
	{
		return false;
	}

	TestTrue(TEXT("Frame 1 queued"), SendFrame(*Sender, Frames[0]) == EO3DSendResult::Queued);
	Receiver->Poll();
	TestEqual(TEXT("A running receiver delivers"), Consumer->Num(), 1);

	TestTrue(TEXT("Frame 2 queued"), SendFrame(*Sender, Frames[1]) == EO3DSendResult::Queued);
	Receiver->Stop();
	TestEqual(TEXT("Poll after Stop processes nothing"), Receiver->Poll(), 0);
	TestEqual(TEXT("A stopped receiver delivers nothing"), Consumer->Num(), 1);

	Sender->Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
