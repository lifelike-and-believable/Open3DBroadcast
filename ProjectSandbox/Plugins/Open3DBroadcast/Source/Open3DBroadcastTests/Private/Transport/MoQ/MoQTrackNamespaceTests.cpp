// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors
//
// MoQ track naming, delivery mode and send-queue limits, observed through the fake moq-ffi
// table (WP-T2, TRF-34). These tests used to initialize a sender and assert TestTrue(true); they
// now check what the transport actually announces, publishes and subscribes to.
// Concurrent sends are covered by Open3DBroadcast.Conformance.MoQ.Send.ConcurrentFromFourThreads.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "Transport/O3DTransportTypes.h"
#include "Testing/MoQTesting.h"
#include "Transport/MoQ/MoQFakeFfi.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

namespace MoQTrackNamespaceTestHelpers
{
	FO3DTransportConfig CreateTestConfig(const FString& RelayUrl, const FString& StreamId)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.Uri = RelayUrl;
		Config.StreamId = StreamId;
		return Config;
	}

	O3DS::SubjectList CreateLargeSubjectList()
	{
		O3DS::SubjectList List;
		for (int32 SubjectIdx = 0; SubjectIdx < 10; ++SubjectIdx)
		{
			const FTCHARToUTF8 SubjectUtf8(*FString::Printf(TEXT("LargeSubject_%d"), SubjectIdx));
			O3DS::Subject* Subject = List.addSubject(std::string(SubjectUtf8.Get(), SubjectUtf8.Length()));
			for (int32 BoneIdx = 0; BoneIdx < 100; ++BoneIdx)
			{
				const FTCHARToUTF8 BoneUtf8(*FString::Printf(TEXT("Bone_%d"), BoneIdx));
				Subject->addTransform(std::string(BoneUtf8.Get(), BoneUtf8.Length()), BoneIdx == 0 ? -1 : 0);
			}
		}
		return List;
	}

	/** Starts a fake-FFI sender (the connect succeeds inline) and asks for an audio sink. */
	TSharedRef<IOpen3DSender> StartSender(const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe>& Fake, const FO3DTransportConfig& Config, TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& OutSink)
	{
		TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
		Sender->Initialize(Config);
		Sender->Start();
		MoQTesting::PumpDispatcher(); // CONNECTING, CONNECTED -> mocap publisher

		FO3DTransportAudioConfig Audio;
		Audio.bEnableAudio = true;
		Audio.SampleRate = 48000;
		Audio.NumChannels = 1;
		OutSink = Sender->CreateAudioSink(Audio); // connected, so the audio publisher is created now
		MoQTesting::PumpDispatcher();
		return Sender;
	}

	const FMoQFakeFfi::FPublisher* FindPublisher(const TArray<FMoQFakeFfi::FPublisher>& Publishers, const FString& Namespace)
	{
		return Publishers.FindByPredicate([&Namespace](const FMoQFakeFfi::FPublisher& P) { return P.Namespace == Namespace; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSeparateTrackNamespacesTest, "Open3DBroadcast.Transport.MoQ.TrackNamespaces.SeparateMocapAudio", O3DB_TEST_FLAGS)
bool FMoQSeparateTrackNamespacesTest::RunTest(const FString& Parameters)
{
	using namespace MoQTrackNamespaceTestHelpers;
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink;
	{
		const TSharedRef<IOpen3DSender> Sender = StartSender(Fake, CreateTestConfig(TEXT("https://fake.relay.invalid:443"), TEXT("session/testTrack")), Sink);

		// StreamId "session/testTrack": namespaces mocap/session and audio/session, track testTrack.
		TestTrue(TEXT("Mocap namespace announced"), Fake->GetAnnounced(1).Contains(TEXT("mocap/session")));
		TestTrue(TEXT("Audio namespace announced"), Fake->GetAnnounced(1).Contains(TEXT("audio/session")));

		const TArray<FMoQFakeFfi::FPublisher> Publishers = Fake->GetPublishers();
		const FMoQFakeFfi::FPublisher* Mocap = FindPublisher(Publishers, TEXT("mocap/session"));
		const FMoQFakeFfi::FPublisher* Audio = FindPublisher(Publishers, TEXT("audio/session"));
		if (TestNotNull(TEXT("Mocap publisher"), Mocap))
		{
			TestEqual(TEXT("Mocap track name"), Mocap->Track, FString(TEXT("testTrack")));
		}
		if (TestNotNull(TEXT("Audio publisher, separate from mocap"), Audio))
		{
			TestEqual(TEXT("Audio track name"), Audio->Track, FString(TEXT("testTrack")));
		}
		Sender->Stop();
	}
	MoQTesting::PumpDispatcher();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQCustomTrackNamingTest, "Open3DBroadcast.Transport.MoQ.TrackNamespaces.CustomNaming", O3DB_TEST_FLAGS)
bool FMoQCustomTrackNamingTest::RunTest(const FString& Parameters)
{
	using namespace MoQTrackNamespaceTestHelpers;
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	FO3DTransportConfig Config = CreateTestConfig(TEXT("https://fake.relay.invalid:443"), TEXT("session/track1"));
	Config.AdvancedParams.Add(TEXT("track_namespace"), TEXT("mocap/customSession"));
	Config.AdvancedParams.Add(TEXT("track_name"), TEXT("characterA"));

	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink;
	{
		const TSharedRef<IOpen3DSender> Sender = StartSender(Fake, Config, Sink);
		const TArray<FMoQFakeFfi::FPublisher> Publishers = Fake->GetPublishers();

		// track_namespace overrides the StreamId session; the audio track swaps the mocap/ prefix.
		const FMoQFakeFfi::FPublisher* Mocap = FindPublisher(Publishers, TEXT("mocap/customSession"));
		const FMoQFakeFfi::FPublisher* Audio = FindPublisher(Publishers, TEXT("audio/customSession"));
		if (TestNotNull(TEXT("Mocap publisher on the custom namespace"), Mocap))
		{
			TestEqual(TEXT("Custom track name"), Mocap->Track, FString(TEXT("characterA")));
		}
		if (TestNotNull(TEXT("Audio publisher on the matching audio namespace"), Audio))
		{
			TestEqual(TEXT("Audio uses the custom track name"), Audio->Track, FString(TEXT("characterA")));
		}
		Sender->Stop();
	}
	MoQTesting::PumpDispatcher();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQDeliveryModeTest, "Open3DBroadcast.Transport.MoQ.TrackNamespaces.DeliveryMode", O3DB_TEST_FLAGS)
bool FMoQDeliveryModeTest::RunTest(const FString& Parameters)
{
	using namespace MoQTrackNamespaceTestHelpers;

	for (const bool bDatagram : { false, true })
	{
		const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
		FO3DTransportConfig Config = CreateTestConfig(TEXT("https://fake.relay.invalid:443"), TEXT("session/modeTrack"));
		if (bDatagram)
		{
			Config.AdvancedParams.Add(TEXT("delivery_mode"), TEXT("datagram"));
		}

		TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink;
		const TSharedRef<IOpen3DSender> Sender = StartSender(Fake, Config, Sink);
		const TArray<FMoQFakeFfi::FPublisher> Publishers = Fake->GetPublishers();
		const FMoQFakeFfi::FPublisher* Mocap = FindPublisher(Publishers, TEXT("mocap/session"));
		const FMoQFakeFfi::FPublisher* Audio = FindPublisher(Publishers, TEXT("audio/session"));
		if (TestNotNull(TEXT("Mocap publisher"), Mocap))
		{
			TestTrue(bDatagram ? TEXT("delivery_mode=datagram publishes mocap as datagrams") : TEXT("Mocap defaults to stream delivery"),
				Mocap->DeliveryMode == (bDatagram ? MOQ_DELIVERY_DATAGRAM : MOQ_DELIVERY_STREAM));
		}
		if (TestNotNull(TEXT("Audio publisher"), Audio))
		{
			TestTrue(TEXT("Audio always uses stream delivery"), Audio->DeliveryMode == MOQ_DELIVERY_STREAM);
		}
		Sender->Stop();
		MoQTesting::PumpDispatcher();
	}
	return true;
}

// TRF-34: the old test sent on a sender that was never started and expected DroppedFrames > 0.
// That path returns false before the queue and never touches Stats, so it could not pass, and it
// never reached the byte cap. Here the sender runs (its connect is held, so nothing is published)
// with the smallest queue the transport allows, and one frame larger than the cap is dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQBackpressureTest, "Open3DBroadcast.Transport.MoQ.Sender.BackpressureByteLimit", O3DB_TEST_FLAGS)
bool FMoQBackpressureTest::RunTest(const FString& Parameters)
{
	using namespace MoQTrackNamespaceTestHelpers;
	// A frame no queue of this size could hold is TooLarge, not a drop (WP-R3, TR-7): the sender
	// pipeline reports it, and the transport neither counts nor logs it. Filling the queue is
	// Conformance.MoQ.Send.BackpressureDropsWithoutBlocking.
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	Fake->bHoldBlockingWork = true;

	const uint64 QueueBytes = MoQTesting::GetBackoffLimits().MinQueueBytes;
	FO3DTransportConfig Config = CreateTestConfig(TEXT("https://fake.relay.invalid:443"), TEXT("session/backpressureTest"));
	Config.AdvancedParams.Add(TEXT("queue_bytes"), FString::Printf(TEXT("%llu"), QueueBytes));

	{
		const TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
		TestTrue(TEXT("Initialize sender"), Sender->Initialize(Config).IsOk());
		TestTrue(TEXT("Start sender (connect held)"), Sender->Start().IsOk());
		// Without a session the worker drops queued frames, which would race the counts below.
		MoQTesting::SenderSetWorkerPaused(*Sender, true);

		TArray<uint8> Small;
		Small.Init(0x5A, 1024);
		TestTrue(TEXT("A frame under the cap is queued"), Sender->SendSerialized(FO3DSendPayload::MakeCopy(Small.GetData(), Small.Num(), TEXT("Subject"), 0.0)) == EO3DSendResult::Queued);
		const int64 DroppedBefore = Sender->GetStats().DroppedFrames;

		TArray<uint8> Oversize;
		Oversize.Init(0xA5, static_cast<int32>(QueueBytes) + 1);
		TestTrue(TEXT("A frame over the byte cap is TooLarge"),
			Sender->SendSerialized(FO3DSendPayload::MakeCopy(Oversize.GetData(), Oversize.Num(), TEXT("Subject"), 0.0)) == EO3DSendResult::TooLarge);
		TestEqual(TEXT("TooLarge is not counted in Stats.DroppedFrames"), Sender->GetStats().DroppedFrames, DroppedBefore);

		MoQTesting::SenderSetWorkerPaused(*Sender, false);
		Sender->Stop();
	}
	Fake->DiscardHeldWork();
	MoQTesting::PumpDispatcher();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQLargePayloadTest, "Open3DBroadcast.Transport.MoQ.Serialization.LargePayload", O3DB_TEST_FLAGS)
bool FMoQLargePayloadTest::RunTest(const FString& Parameters)
{
	using namespace MoQTrackNamespaceTestHelpers;
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink;
	{
		const TSharedRef<IOpen3DSender> Sender = StartSender(Fake, CreateTestConfig(TEXT("https://fake.relay.invalid:443"), TEXT("session/largePayloadTest")), Sink);

		O3DS::SubjectList LargeList = CreateLargeSubjectList();
		std::vector<char> Buffer;
		LargeList.Serialize(Buffer, 1.0);
		AddInfo(FString::Printf(TEXT("Large payload size: %d bytes"), static_cast<int32>(Buffer.size())));

		TestTrue(TEXT("Large payload is queued"),
			Sender->SendSerialized(FO3DSendPayload::MakeCopy(reinterpret_cast<const uint8*>(Buffer.data()), static_cast<int32>(Buffer.size()), TEXT("LargeSubject_0"), 1.0)) == EO3DSendResult::Queued);
		TestTrue(TEXT("The worker publishes it"), O3DTests::PollUntil(5.0, [&Fake]() { return Fake->GetPublishCalls() >= 1; }));
		TestEqual(TEXT("Nothing dropped"), Sender->GetStats().DroppedFrames, static_cast<int64>(0));
		Sender->Stop();
	}
	MoQTesting::PumpDispatcher();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverMultiTrackTest, "Open3DBroadcast.Transport.MoQ.Receiver.MultipleTrackSubscriptions", O3DB_TEST_FLAGS)
bool FMoQReceiverMultiTrackTest::RunTest(const FString& Parameters)
{
	using namespace MoQTrackNamespaceTestHelpers;

	class FNullAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta&, const uint8*, int32) override {}
	};

	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const FO3DTransportConfig Config = CreateTestConfig(TEXT("https://fake.relay.invalid:443"), TEXT("session/multiTrackTest"));
	const TSharedRef<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FNullAudioSink, ESPMode::ThreadSafe>();
	{
		const TSharedRef<IOpen3DReceiver> Receiver = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 1);
		TestTrue(TEXT("Initialize receiver"), Receiver->Initialize(Config).IsOk());
		Receiver->SetAudioSink(AudioSink, Config.Audio);
		// The receiver holds its consumer weakly (until ADR 0007 step 5), so the test keeps it alive.
		const TSharedRef<FO3DRecordingFrameConsumer> FrameConsumer = MakeShared<FO3DRecordingFrameConsumer>();
		Receiver->SetConsumer(FrameConsumer);
		TestTrue(TEXT("Start receiver"), Receiver->Start().IsOk());
		MoQTesting::PumpDispatcher(); // CONNECTED -> mocap and audio subscriptions

		const TArray<FString> Subscriptions = Fake->GetLiveSubscriptions();
		TestTrue(TEXT("Subscribed to the mocap track"), Subscriptions.Contains(TEXT("mocap/session|multiTrackTest")));
		TestTrue(TEXT("Subscribed to the audio track"), Subscriptions.Contains(TEXT("audio/session|multiTrackTest")));
		Receiver->Stop();
	}
	MoQTesting::PumpDispatcher();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQRelayUrlVariationsTest, "Open3DBroadcast.Transport.MoQ.Configuration.RelayUrlVariations", O3DB_TEST_FLAGS)
bool FMoQRelayUrlVariationsTest::RunTest(const FString& Parameters)
{
	using namespace MoQTrackNamespaceTestHelpers;

	const TCHAR* Urls[] = { TEXT("https://relay.example.com:4443"), TEXT("http://localhost:4443"), TEXT("https://relay.example.com:8443") };
	for (const TCHAR* Url : Urls)
	{
		const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
		const TSharedRef<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 1);
		TestTrue(*FString::Printf(TEXT("Relay URL %s is accepted"), Url), Sender->Initialize(CreateTestConfig(Url, TEXT("session/test"))).IsOk());
		Sender->Stop();
	}
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
