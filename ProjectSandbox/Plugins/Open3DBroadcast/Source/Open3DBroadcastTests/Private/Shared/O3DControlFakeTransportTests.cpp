// Copyright Lifelike & Believable. All Rights Reserved.

//
// ADR 0011 (CTL-2): the control path end to end through the fake transports, with the real core
// on both ends: ControlPublisher -> WriteControlEnvelope -> IOpen3DSender::SendControl -> link ->
// IOpen3DReceiver -> IO3DReceiverControlSink -> ControlReceiver -> O3DControl::FromCore ->
// FO3DControlBus. Control rides in-band next to mocap and must not disturb it.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "O3DControlBus.h"
#include "O3DControlConvert.h"
#include "O3DTestFakes.h"
#include "O3DUnifiedMessage.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

namespace O3DControlFakeTransportTests
{
	class FCountingConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& /*Subject*/, const TArray<uint8>& Buffer, double /*TimestampSeconds*/) override
		{
			Frames.Add(Buffer);
		}
		TArray<TArray<uint8>> Frames;
	};

	/** Runs the core receiver on what the transport hands over and publishes to the bus (what CTL-4's receiver source does). */
	class FBusControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& StreamId, double ReceiveTimeSec) override
		{
			check(IsInGameThread()); // the fake delivers in Poll, on the calling thread
			std::vector<O3DS::Control::Change> Changes;
			Receiver.Submit(Payload.GetData(), static_cast<size_t>(Payload.Num()), ReceiveTimeSec, Changes);
			for (const O3DS::Control::Change& Change : Changes)
			{
				FO3DControlBus::Publish(O3DControl::FromCore(Change, StreamId));
			}
			++Payloads;
		}
		O3DS::Control::ControlReceiver Receiver;
		int32 Payloads = 0;
	};

	/** Sends everything the publisher produced this tick; returns how many the transport accepted. */
	int32 Flush(O3DS::Control::ControlPublisher& Publisher, IOpen3DSender& Sender, double NowSec)
	{
		std::vector<O3DS::Control::OutgoingMessage> Out;
		Publisher.Tick(NowSec, static_cast<uint64_t>(NowSec * 1.0e6), 0, Out);
		int32 Accepted = 0;
		for (const O3DS::Control::OutgoingMessage& Message : Out)
		{
			TArray<uint8> Envelope;
			if (!O3DS::WriteControlEnvelope(TConstArrayView<uint8>(Message.bytes.data(), static_cast<int32>(Message.bytes.size())), NowSec, Envelope))
			{
				continue;
			}
			if (Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued)
			{
				++Accepted;
			}
			else
			{
				Publisher.OnSendRefused(Message, NowSec);
			}
		}
		return Accepted;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlFakeEndToEndTest, "Open3DBroadcast.Shared.Control.FakeTransport.EndToEndBesideMocap", O3DB_TEST_FLAGS)
bool FO3DControlFakeEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlFakeTransportTests;
	FO3DControlBus::ResetForTesting();
	TArray<FO3DControlChange> Seen;
	FO3DControlBus::OnChange().AddLambda([&Seen](const FO3DControlChange& Change) { Seen.Add(Change); });

	const FO3DFakeLinkRef Link = MakeShared<FO3DFakeLink, ESPMode::ThreadSafe>();
	FO3DFakeSender Sender(Link);
	FO3DFakeReceiver Receiver(Link);
	const TSharedRef<FCountingConsumer> Consumer = MakeShared<FCountingConsumer>();
	const TSharedRef<FBusControlSink, ESPMode::ThreadSafe> Sink = MakeShared<FBusControlSink, ESPMode::ThreadSafe>();

	FO3DTransportConfig Config;
	Config.StreamId = TEXT("stage");
	TestTrue(TEXT("Both advertise control"), Sender.SupportsControl() && Receiver.SupportsControl());
	TestTrue(TEXT("Sender initializes"), Sender.Initialize(Config).IsOk() && Sender.Start().IsOk());
	Receiver.Initialize(Config);
	Receiver.SetConsumer(Consumer);
	Receiver.SetControlSink(Sink);
	TestTrue(TEXT("Receiver starts"), Receiver.Start().IsOk());

	O3DS::Control::ControlPublisher Publisher("0a1b2c3d4e5f60718293a4b5c6d7e8f9", "BP_Stage");
	Publisher.Start(1000);
	Publisher.SetValue("env.fog_density", "", O3DS::Control::Value::MakeDouble(0.35));
	Publisher.SetValue("char.emotion", "Hero", O3DS::Control::Value::MakeName("joy"));
	Publisher.FireEvent("vfx.muzzle_flash", "Hero", O3DS::Control::Value::MakeName("left_hand"), 1000000);

	// Mocap and control interleaved on one link.
	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("Hero"), 5);
	for (const TArray<uint8>& Frame : Frames)
	{
		Sender.SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Hero"), 0.0));
	}
	TestTrue(TEXT("Control accepted"), Flush(Publisher, Sender, 1.0) > 0);
	for (const TArray<uint8>& Frame : Frames)
	{
		Sender.SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Hero"), 0.0));
	}

	const int32 FramesPolled = Receiver.Poll();
	TestEqual(TEXT("Poll counts only mocap frames"), FramesPolled, Frames.Num() * 2);
	TestEqual(TEXT("Every mocap frame reached the consumer unchanged"), Consumer->Frames.Num(), Frames.Num() * 2);
	if (Consumer->Frames.Num() == Frames.Num() * 2)
	{
		TestTrue(TEXT("Frame bytes intact"), Consumer->Frames[0] == Frames[0] && Consumer->Frames.Last() == Frames.Last());
	}
	TestEqual(TEXT("Sender counts only frames as sent"), Sender.GetStats().FramesSent, static_cast<int64>(Frames.Num() * 2));
	TestTrue(TEXT("Control reached the sink"), Sink->Payloads > 0 && Receiver.GetControlDelivered() == Sink->Payloads);

	const FString SourceId(TEXT("0a1b2c3d4e5f60718293a4b5c6d7e8f9"));
	const FO3DControlValue* Fog = FO3DControlBus::FindValue(SourceId, TEXT("env.fog_density"), FString());
	TestTrue(TEXT("Value on the bus"), Fog != nullptr && *Fog == FO3DControlValue::MakeFloat(0.35));
	const FO3DControlValue* Emotion = FO3DControlBus::FindValue(SourceId, TEXT("char.emotion"), TEXT("Hero"));
	TestTrue(TEXT("Targeted value on the bus"), Emotion != nullptr && *Emotion == FO3DControlValue::MakeName(TEXT("joy")));

	int32 Events = 0;
	for (const FO3DControlChange& Change : Seen)
	{
		if (Change.Kind == FO3DControlChange::EKind::Event)
		{
			++Events;
			TestEqual(TEXT("Event name"), Change.Name, FString(TEXT("vfx.muzzle_flash")));
			TestEqual(TEXT("Event target"), Change.Meta.TargetSubject, FString(TEXT("Hero")));
			TestEqual(TEXT("Stream id from the transport"), Change.Meta.StreamId, FString(TEXT("stage")));
			TestEqual(TEXT("Source name"), Change.Meta.SourceName, FString(TEXT("BP_Stage")));
		}
	}
	TestEqual(TEXT("The event fired once"), Events, 1);

	Receiver.Stop();
	TestFalse(TEXT("Stop releases the control sink"), Receiver.HasControlSink());
	Sender.Stop();
	FO3DControlBus::ResetForTesting();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlFakeContractTest, "Open3DBroadcast.Shared.Control.FakeTransport.SendControlContract", O3DB_TEST_FLAGS)
bool FO3DControlFakeContractTest::RunTest(const FString& Parameters)
{
	const FO3DFakeLinkRef Link = MakeShared<FO3DFakeLink, ESPMode::ThreadSafe>();
	FO3DFakeSender Sender(Link);

	const TArray<uint8> Payload = { 1, 2, 3, 4 };
	TArray<uint8> Envelope;
	O3DS::WriteControlEnvelope(Payload, 1.0, Envelope);

	TestTrue(TEXT("NotRunning before Initialize"), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::NotRunning);
	FO3DTransportConfig Config;
	Sender.Initialize(Config);
	TestTrue(TEXT("NotRunning before Start"), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::NotRunning);
	Sender.Start();
	TestTrue(TEXT("Queued while running"), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued);

	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("Hero"), 1);
	TestTrue(TEXT("A mocap frame is not a control envelope (Invalid)"), Sender.SendControl(Frames[0].GetData(), Frames[0].Num()) == EO3DSendResult::Invalid);
	TestTrue(TEXT("Null is Invalid"), Sender.SendControl(nullptr, 10) == EO3DSendResult::Invalid);

	Sender.SetMaxQueued(1); // one already queued
	TestTrue(TEXT("DroppedBackpressure when the queue is full (the publisher retries)"), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::DroppedBackpressure);
	TestEqual(TEXT("A refused control send is not a dropped frame"), Sender.GetStats().DroppedFrames, static_cast<int64>(0));

	Sender.Stop();
	TestTrue(TEXT("NotRunning after Stop"), Sender.SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::NotRunning);
	TestEqual(TEXT("One accepted"), Sender.GetRecordedControl().Num(), 1);

	// A malformed control envelope is dropped by the receiver, not passed to the frame consumer.
	FO3DFakeReceiver Receiver(Link);
	Receiver.Initialize(Config);
	const TSharedRef<O3DControlFakeTransportTests::FCountingConsumer> Consumer = MakeShared<O3DControlFakeTransportTests::FCountingConsumer>();
	Receiver.SetConsumer(Consumer);
	Receiver.Start();
	Link->Drain();
	TArray<uint8> Malformed;
	O3DS::CreateUnifiedMessage(O3DS::EUnifiedKind::Control, O3DS::EUnifiedCodec::Opus, Payload.GetData(), Payload.Num(), 1.0, Malformed);
	Receiver.Enqueue(Malformed);
	TestEqual(TEXT("Not a frame"), Receiver.Poll(), 0);
	TestEqual(TEXT("Consumer untouched"), Consumer->Frames.Num(), 0);
	TestEqual(TEXT("Counted as rejected"), Receiver.GetControlRejected(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
