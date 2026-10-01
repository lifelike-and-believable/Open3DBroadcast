// Copyright Lifelike & Believable. All Rights Reserved.

// ADR 0011 (CTL-5): control on MoQ. The sender announces a control track on every connect and
// publishes control envelopes on it with stream delivery; the receiver subscribes to it only
// while a control sink is set. Control is never a frame on either side. Everything runs on the
// fake moq-ffi table (MoQFakeFfi.h), reached only through Testing/MoQTesting.h.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "Misc/ScopeLock.h"
#include "O3DUnifiedMessage.h"
#include "Testing/MoQTesting.h"
#include "Transport/MoQ/MoQFakeFfi.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportTypes.h"

namespace MoQControlTests
{
	FO3DTransportConfig MakeConfig()
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.Uri = TEXT("https://fake.relay.invalid:443"); // only the fake FFI ever sees it
		Config.StreamId = TEXT("ctl5/actor");                // mocap/ctl5, control/ctl5, track "actor"
		return Config;
	}

	const FString ControlNamespace = TEXT("control/ctl5");
	const FString ControlSubscription = TEXT("control/ctl5|actor");

	TArray<uint8> MakePayload(int32 Index)
	{
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(24 + Index * 41);
		for (int32 Byte = 0; Byte < Payload.Num(); ++Byte)
		{
			Payload[Byte] = static_cast<uint8>((Index * 29 + Byte * 5 + 1) & 0xFF);
		}
		return Payload;
	}

	TArray<uint8> MakeEnvelope(int32 Index)
	{
		TArray<uint8> Envelope;
		O3DS::WriteControlEnvelope(MakePayload(Index), 1.0 + Index, Envelope);
		return Envelope;
	}

	/** Records control payloads; SubmitControl may run on any thread. */
	class FMoQControlRecordingSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& StreamId, double /*ReceiveTimeSec*/) override
		{
			FScopeLock Lock(&Mutex);
			Payloads.Emplace(Payload.GetData(), Payload.Num());
			LastStreamId = StreamId;
		}

		TArray<TArray<uint8>> Get() const { FScopeLock Lock(&Mutex); return Payloads; }
		int32 Num() const { FScopeLock Lock(&Mutex); return Payloads.Num(); }
		FString GetLastStreamId() const { FScopeLock Lock(&Mutex); return LastStreamId; }

	private:
		mutable FCriticalSection Mutex;
		TArray<TArray<uint8>> Payloads;
		FString LastStreamId;
	};

	/** Counts frames; control must never reach it. */
	class FMoQControlFrameCounter final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& /*Subject*/, const TArray<uint8>& /*Buffer*/, double /*TimestampSeconds*/) override
		{
			FScopeLock Lock(&Mutex);
			++Frames;
		}

		int32 Num() const { FScopeLock Lock(&Mutex); return Frames; }

	private:
		mutable FCriticalSection Mutex;
		int32 Frames = 0;
	};

	const FMoQFakeFfi::FPublisher* FindPublisher(const TArray<FMoQFakeFfi::FPublisher>& Publishers, const FString& Namespace)
	{
		return Publishers.FindByPredicate([&Namespace](const FMoQFakeFfi::FPublisher& P) { return P.Namespace == Namespace && !P.bDestroyed; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQControlTrackAnnouncedTest, "Open3DBroadcast.Transport.MoQ.Control.TrackAnnouncedOnConnect", O3DB_TEST_FLAGS)
bool FMoQControlTrackAnnouncedTest::RunTest(const FString& Parameters)
{
	using namespace MoQControlTests;
	MoQTesting::InitializeDispatcher();

	for (const bool bDatagram : { false, true })
	{
		const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
		FO3DTransportConfig Config = MakeConfig();
		if (bDatagram)
		{
			Config.AdvancedParams.Add(TEXT("delivery_mode"), TEXT("datagram"));
		}
		{
			const TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
			TestTrue(TEXT("Sender supports control"), Sender->SupportsControl());
			TestTrue(TEXT("Initialize"), Sender->Initialize(Config));

			const TArray<uint8> Envelope = MakeEnvelope(0);
			TestFalse(TEXT("SendControl before Start is refused"), Sender->SendControl(Envelope.GetData(), Envelope.Num()));

			TestTrue(TEXT("Start"), Sender->Start());
			MoQFakeTest::Pump(); // CONNECTED -> mocap and control publishers, no SendControl needed

			TestTrue(TEXT("Control namespace announced on connect"), Fake->GetAnnounced(1).Contains(ControlNamespace));
			const TArray<FMoQFakeFfi::FPublisher> Publishers = Fake->GetPublishers();
			const FMoQFakeFfi::FPublisher* Control = FindPublisher(Publishers, ControlNamespace);
			if (TestNotNull(TEXT("Control publisher"), Control))
			{
				TestEqual(TEXT("Control uses the stream's track name"), Control->Track, FString(TEXT("actor")));
				TestTrue(TEXT("Control always uses stream delivery, whatever delivery_mode says"), Control->DeliveryMode == MOQ_DELIVERY_STREAM);
			}
			TestTrue(TEXT("SendControl accepted once the control track is announced"), Sender->SendControl(Envelope.GetData(), Envelope.Num()));
			const TArray<uint8> NotControl = MakePayload(0);
			TestFalse(TEXT("Bytes that are not a control envelope are refused"), Sender->SendControl(NotControl.GetData(), NotControl.Num()));

			Sender->Stop();
			TestFalse(TEXT("SendControl after Stop is refused"), Sender->SendControl(Envelope.GetData(), Envelope.Num()));
		}
		MoQFakeTest::Pump();
		TestEqual(TEXT("Every publisher destroyed"), Fake->GetPublishersDestroyed(), Fake->GetPublishersCreated());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQControlCustomNamespaceTest, "Open3DBroadcast.Transport.MoQ.Control.NeverSharesTheMocapTrack", O3DB_TEST_FLAGS)
bool FMoQControlCustomNamespaceTest::RunTest(const FString& Parameters)
{
	using namespace MoQControlTests;
	MoQTesting::InitializeDispatcher();

	// mocap/ is swapped for control/; a namespace with no known prefix gets control/ prepended.
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{ TEXT("mocap/customSession"), TEXT("control/customSession") },
		{ TEXT("studio/stageA"), TEXT("control/studio/stageA") },
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : Cases)
	{
		const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
		FO3DTransportConfig Config = MakeConfig();
		Config.AdvancedParams.Add(TEXT("track_namespace"), Case.Key);
		{
			const TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 2);
			TestTrue(TEXT("Initialize"), Sender->Initialize(Config));
			TestTrue(TEXT("Start"), Sender->Start());
			MoQFakeTest::Pump();
			TestTrue(*FString::Printf(TEXT("track_namespace '%s' puts control on '%s'"), Case.Key, Case.Value),
				Fake->GetAnnounced(1).Contains(FString(Case.Value)));
			Sender->Stop();
		}
		MoQFakeTest::Pump();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQControlSubscribeOnlyWithSinkTest, "Open3DBroadcast.Transport.MoQ.Control.SubscribesOnlyWithSink", O3DB_TEST_FLAGS)
bool FMoQControlSubscribeOnlyWithSinkTest::RunTest(const FString& Parameters)
{
	using namespace MoQControlTests;
	MoQTesting::InitializeDispatcher();

	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<FMoQControlRecordingSink, ESPMode::ThreadSafe> Sink = MakeShared<FMoQControlRecordingSink, ESPMode::ThreadSafe>();
	{
		const TSharedRef<IOpen3DReceiver> Receiver = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 3);
		TestTrue(TEXT("Receiver supports control"), Receiver->SupportsControl());
		TestTrue(TEXT("Initialize"), Receiver->Initialize(MakeConfig()));
		TestTrue(TEXT("Start"), Receiver->Start());
		MoQFakeTest::Pump(); // CONNECTED -> mocap subscription only
		TestFalse(TEXT("No control subscription without a sink"), Fake->GetLiveSubscriptions().Contains(ControlSubscription));

		Receiver->SetControlSink(Sink);
		TestTrue(TEXT("A sink set while connected subscribes"), Fake->GetLiveSubscriptions().Contains(ControlSubscription));

		Receiver->SetControlSink(nullptr);
		TestFalse(TEXT("Clearing the sink releases the subscription"), Fake->GetLiveSubscriptions().Contains(ControlSubscription));
		Receiver->Stop();
	}
	MoQFakeTest::Pump();

	{
		const TSharedRef<IOpen3DReceiver> Receiver = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 4);
		TestTrue(TEXT("Initialize"), Receiver->Initialize(MakeConfig()));
		Receiver->SetControlSink(Sink); // before Start, as the interface asks
		TestTrue(TEXT("Start"), Receiver->Start());
		MoQFakeTest::Pump();
		TestTrue(TEXT("A sink set before Start subscribes on connect"), Fake->GetLiveSubscriptions().Contains(ControlSubscription));
		Receiver->Stop();
		TestFalse(TEXT("Stop releases the control subscription"), Fake->GetLiveSubscriptions().Contains(ControlSubscription));
	}
	MoQFakeTest::Pump();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQControlRoundTripTest, "Open3DBroadcast.Transport.MoQ.Control.RoundTripIsNotAFrame", O3DB_TEST_FLAGS)
bool FMoQControlRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MoQControlTests;
	MoQTesting::InitializeDispatcher();

	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const TSharedRef<FMoQControlRecordingSink, ESPMode::ThreadSafe> Sink = MakeShared<FMoQControlRecordingSink, ESPMode::ThreadSafe>();
	const TSharedRef<FMoQControlFrameCounter> Consumer = MakeShared<FMoQControlFrameCounter>();
	constexpr int32 NumControl = 5;
	{
		const TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 5);
		const TSharedRef<IOpen3DReceiver> Receiver = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 6);
		TestTrue(TEXT("Sender initializes"), Sender->Initialize(MakeConfig()));
		TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(MakeConfig()));
		Receiver->SetConsumer(Consumer);
		Receiver->SetControlSink(Sink);
		TestTrue(TEXT("Sender starts"), Sender->Start());
		MoQFakeTest::Pump(); // the sender announces first
		TestTrue(TEXT("Receiver starts"), Receiver->Start());
		MoQFakeTest::Pump();

		for (int32 Index = 0; Index < NumControl; ++Index)
		{
			const TArray<uint8> Envelope = MakeEnvelope(Index);
			TestTrue(*FString::Printf(TEXT("Control %d accepted"), Index), Sender->SendControl(Envelope.GetData(), Envelope.Num()));
		}

		const bool bArrived = O3DTests::PollUntil(5.0,
			[&Sink]() { return Sink->Num() >= NumControl; },
			[&Receiver, &Sender]() { MoQFakeTest::Pump(); Receiver->Poll(); Sender->Tick(0.0f); });
		TestTrue(TEXT("Every control payload arrived"), bArrived);

		const TArray<TArray<uint8>> Received = Sink->Get();
		TestEqual(TEXT("Each exactly once"), Received.Num(), NumControl);
		for (int32 Index = 0; Index < Received.Num() && Index < NumControl; ++Index)
		{
			TestTrue(*FString::Printf(TEXT("Control %d byte-exact and in order"), Index), Received[Index] == MakePayload(Index));
		}
		TestEqual(TEXT("Delivered under the receiver's stream id"), Sink->GetLastStreamId(), FString(TEXT("ctl5/actor")));
		TestEqual(TEXT("No control reached the frame consumer"), Consumer->Num(), 0);

		const FO3DTransportStats SenderStats = Sender->GetStats();
		const FO3DTransportStats ReceiverStats = Receiver->GetStats();
		TestEqual(TEXT("Control is not a sent frame"), SenderStats.FramesSent, static_cast<int64>(0));
		TestEqual(TEXT("Control is not a sent byte"), SenderStats.BytesSent, static_cast<int64>(0));
		TestEqual(TEXT("Control is not a received frame"), ReceiverStats.FramesReceived, static_cast<int64>(0));
		TestEqual(TEXT("Nothing counted as dropped"), SenderStats.DroppedFrames + ReceiverStats.DroppedFrames, static_cast<int64>(0));

		Receiver->Stop();
		Sender->Stop();
	}
	MoQFakeTest::Pump();
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
