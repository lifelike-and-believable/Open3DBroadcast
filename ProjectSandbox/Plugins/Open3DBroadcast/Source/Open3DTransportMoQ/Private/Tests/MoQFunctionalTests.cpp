// Copyright (c) Open3DStream Contributors
//
// WP-S8 tests for MoQ (TRF-8, TRF-9, TRF-11, TRF-13, TRF-20, TRF-29, TRF-37, TRF-39).
//
// Everything runs through the fake moq-ffi table (MoQFakeFfi.h, ADR 0006 F2): no relay, no
// network, no sleeps. Blocking FFI calls run inline or are held by the fake, FFI callbacks are
// delivered by draining the dispatcher on the game thread, and time comes from a manual clock.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "O3DAudioSerialization.h"
#include "O3DReceiverInterface.h"
#include "O3DTransportTypes.h"
#include "Receiver/MoQReceiver.h"
#include "Sender/MoQSender.h"
#include "Shared/MoQAsyncDispatcher.h"
#include "Shared/MoQHelpers.h"
#include "Shared/MoQSessionWrapper.h"
#include "Testing/O3DLifetimeTestUtils.h"
#include "Tests/MoQFakeFfi.h"

#include <atomic>

#if O3D_WITH_TRANSPORT_MOQ

namespace
{
	FO3DTransportConfig MakeFakeConfig()
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.Uri = TEXT("https://fake.relay.invalid:443"); // only the fake FFI ever sees it
		Config.StreamId = TEXT("wp_s8/actor");                // mocap/wp_s8, audio/wp_s8, track "actor"
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		return Config;
	}

	const FString MocapNamespace = TEXT("mocap/wp_s8");
	const FString AudioNamespace = TEXT("audio/wp_s8");

	bool HasDuplicates(const TArray<FString>& Values)
	{
		TSet<FString> Seen;
		for (const FString& Value : Values)
		{
			bool bAlreadyInSet = false;
			Seen.Add(Value, &bAlreadyInSet);
			if (bAlreadyInSet)
			{
				return true;
			}
		}
		return false;
	}

	/** Receiver audio sink that records what it is given (game thread only in these tests). */
	class FRecordingAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes) override
		{
			++Frames;
			LastBytes = NumBytes;
			LastChannels = Meta.NumChannels;
			LastLabel = Meta.StreamLabel;
		}

		int32 Frames = 0;
		int32 LastBytes = 0;
		int32 LastChannels = 0;
		FString LastLabel;
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQBackoffJitterTest, "Open3DBroadcast.Transport.MoQ.Backoff.CappedJitter", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQBackoffJitterTest::RunTest(const FString& Parameters)
{
	for (int32 Failures = 0; Failures <= 12; ++Failures)
	{
		const double Base = MoQHelpers::ComputeReconnectDelaySeconds(Failures);
		for (uint64 Seed = 0; Seed < 64; ++Seed)
		{
			const double Delay = MoQHelpers::ComputeBackoffDelaySeconds(Failures, Seed);
			if (Delay < (1.0 - MoQHelpers::kBackoffJitterFraction) * Base - 1e-9 || Delay > Base + 1e-9)
			{
				AddError(FString::Printf(TEXT("Delay %f for %d failures (seed %llu) outside [%f, %f]"), Delay, Failures, Seed,
					(1.0 - MoQHelpers::kBackoffJitterFraction) * Base, Base));
			}
			TestEqual(TEXT("Same seed gives the same delay"), MoQHelpers::ComputeBackoffDelaySeconds(Failures, Seed), Delay);
		}
		TestTrue(TEXT("Delay is capped"), Base <= MoQHelpers::kMaxReconnectDelaySeconds);
	}

	TestTrue(TEXT("Backoff grows with failures"), MoQHelpers::ComputeReconnectDelaySeconds(3) > MoQHelpers::ComputeReconnectDelaySeconds(1));
	TestEqual(TEXT("Backoff reaches the cap"), MoQHelpers::ComputeReconnectDelaySeconds(20), MoQHelpers::kMaxReconnectDelaySeconds);

	bool bSeedsDiffer = false;
	for (uint64 Seed = 1; Seed < 16 && !bSeedsDiffer; ++Seed)
	{
		bSeedsDiffer = MoQHelpers::ComputeBackoffDelaySeconds(4, Seed) != MoQHelpers::ComputeBackoffDelaySeconds(4, 0);
	}
	TestTrue(TEXT("Different seeds spread retries out"), bSeedsDiffer);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQConnectTimeoutOptionTest, "Open3DBroadcast.Transport.MoQ.Options.ConnectTimeout", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQConnectTimeoutOptionTest::RunTest(const FString& Parameters)
{
	FO3DTransportConfig Config = MakeFakeConfig();
	TestEqual(TEXT("Default"), MoQHelpers::ResolveConnectTimeoutSeconds(Config), MoQHelpers::kDefaultConnectTimeoutSeconds);

	Config.AdvancedParams.Add(TEXT("connect_timeout"), TEXT("5"));
	TestEqual(TEXT("connect_timeout"), MoQHelpers::ResolveConnectTimeoutSeconds(Config), 5.0);

	Config.AdvancedParams.Reset();
	Config.AdvancedParams.Add(TEXT("moq.connect_timeout"), TEXT("2.5"));
	TestEqual(TEXT("moq.connect_timeout"), MoQHelpers::ResolveConnectTimeoutSeconds(Config), 2.5);

	Config.AdvancedParams.Reset();
	Config.AdvancedParams.Add(TEXT("connect_timeout"), TEXT("0.01"));
	TestEqual(TEXT("Clamped low"), MoQHelpers::ResolveConnectTimeoutSeconds(Config), MoQHelpers::kMinConnectTimeoutSeconds);

	Config.AdvancedParams.Reset();
	Config.AdvancedParams.Add(TEXT("connect_timeout"), TEXT("100000"));
	TestEqual(TEXT("Clamped high"), MoQHelpers::ResolveConnectTimeoutSeconds(Config), MoQHelpers::kMaxConnectTimeoutSeconds);

	Config.AdvancedParams.Reset();
	Config.AdvancedParams.Add(TEXT("connect_timeout"), TEXT("soon"));
	TestEqual(TEXT("Garbage uses the default"), MoQHelpers::ResolveConnectTimeoutSeconds(Config), MoQHelpers::kDefaultConnectTimeoutSeconds);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRF-8: disconnect, reconnect, re-announce
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderReconnectReannounceTest, "Open3DBroadcast.Transport.MoQ.Sender.ReconnectReannounces", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderReconnectReannounceTest::RunTest(const FString& Parameters)
{
	// The unexpected disconnect below is logged once as a warning.
	AddExpectedError(TEXT("MoQ session state"), EAutomationExpectedMessageFlags::Contains, 1);

	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	MoQFakeTest::FManualClock Clock;
	const FO3DTransportConfig Config = MakeFakeConfig();

	{
		FO3DMoQSender Sender(Fake->MakeApi(), Clock.AsFunction(), /*JitterSeed=*/42);
		TestTrue(TEXT("Initialize"), Sender.Initialize(Config));
		TestTrue(TEXT("Start"), Sender.Start());
		MoQFakeTest::Pump(); // CONNECTING, CONNECTED -> mocap publisher

		TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink = Sender.CreateAudioSink(Config.Audio);
		TestTrue(TEXT("Audio sink created"), Sink.IsValid());

		TestEqual(TEXT("One client so far"), Fake->GetClientsCreated(), 1);
		TestEqual(TEXT("Client 1 announced mocap and audio once each"), Fake->GetAnnounced(1).Num(), 2);
		TestTrue(TEXT("Client 1 announced mocap"), Fake->GetAnnounced(1).Contains(MocapNamespace));
		TestTrue(TEXT("Client 1 announced audio"), Fake->GetAnnounced(1).Contains(AudioNamespace));

		// Ticking while connected must not announce again.
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Sender.Tick(0.0f);
		}
		TestEqual(TEXT("No extra announces while connected"), Fake->GetAnnounceLog().Num(), 2);

		// Unexpected drop reported by moq-ffi.
		TestTrue(TEXT("Fake fired DISCONNECTED"), Fake->FireConnectionState(1, MOQ_STATE_DISCONNECTED));
		MoQFakeTest::Pump();
		TestEqual(TEXT("Both publishers released on disconnect"), Fake->GetPublishersDestroyed(), 2);

		// Base delay for zero failures is 0.5 s, jittered down to at least 0.375 s.
		Clock.Advance(0.25);
		Sender.Tick(0.0f);
		TestEqual(TEXT("No reconnect before the backoff"), Fake->GetBlockingLaunches(), 1);

		Clock.Advance(0.25);
		Sender.Tick(0.0f);
		TestEqual(TEXT("Reconnect after the backoff"), Fake->GetBlockingLaunches(), 2);
		MoQFakeTest::Pump(); // CONNECTED on client 2 -> publishers again

		TestEqual(TEXT("Reconnect used a new client"), Fake->GetClientsCreated(), 2);
		TestTrue(TEXT("Client 2 re-announced mocap"), Fake->GetAnnounced(2).Contains(MocapNamespace));
		TestTrue(TEXT("Client 2 re-announced audio"), Fake->GetAnnounced(2).Contains(AudioNamespace));
		TestFalse(TEXT("Client 1 announced nothing twice"), HasDuplicates(Fake->GetAnnounced(1)));
		TestFalse(TEXT("Client 2 announced nothing twice"), HasDuplicates(Fake->GetAnnounced(2)));
		TestEqual(TEXT("Four announces in total"), Fake->GetAnnounceLog().Num(), 4);
		TestTrue(TEXT("The old client was destroyed"), Fake->IsClientDestroyed(1));

		Sender.Stop();
	}

	MoQFakeTest::Pump();
	TestEqual(TEXT("Every publisher destroyed"), Fake->GetPublishersDestroyed(), Fake->GetPublishersCreated());
	TestTrue(TEXT("Client 2 destroyed after Stop"), Fake->IsClientDestroyed(2));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRF-11: a connect that never completes times out and is retried with backoff
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderConnectTimeoutTest, "Open3DBroadcast.Transport.MoQ.Sender.ConnectTimeoutRetriesWithBackoff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderConnectTimeoutTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("did not complete within"), EAutomationExpectedMessageFlags::Contains, 2);

	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	Fake->bHoldBlockingWork = true; // moq_connect never returns until the test says so
	MoQFakeTest::FManualClock Clock;
	FO3DTransportConfig Config = MakeFakeConfig();
	Config.AdvancedParams.Add(TEXT("connect_timeout"), TEXT("5"));

	{
		FO3DMoQSender Sender(Fake->MakeApi(), Clock.AsFunction(), /*JitterSeed=*/7);
		TestTrue(TEXT("Initialize"), Sender.Initialize(Config));
		TestTrue(TEXT("Start"), Sender.Start());
		TestEqual(TEXT("First attempt launched"), Fake->GetBlockingLaunches(), 1);

		// Clock steps are exact binary fractions so the comparisons are exact.
		Clock.Advance(4.75);
		Sender.Tick(0.0f);
		TestEqual(TEXT("Still waiting before the timeout"), Fake->GetBlockingLaunches(), 1);

		Clock.Advance(0.25);
		Sender.Tick(0.0f); // timeout: 1 failure, next attempt in [0.75, 1.0] s
		TestEqual(TEXT("No immediate retry after the timeout"), Fake->GetBlockingLaunches(), 1);

		Clock.Advance(0.5);
		Sender.Tick(0.0f);
		TestEqual(TEXT("No retry inside the first backoff"), Fake->GetBlockingLaunches(), 1);

		Clock.Advance(0.5);
		Sender.Tick(0.0f);
		TestEqual(TEXT("Retry after the first backoff"), Fake->GetBlockingLaunches(), 2);
		TestEqual(TEXT("Retry uses a fresh client"), Fake->GetClientsCreated(), 2);

		Clock.Advance(5.0);
		Sender.Tick(0.0f); // second timeout: 2 failures, next attempt in [1.5, 2.0] s

		Clock.Advance(1.25);
		Sender.Tick(0.0f);
		TestEqual(TEXT("No retry inside the second, longer backoff"), Fake->GetBlockingLaunches(), 2);

		Clock.Advance(0.75);
		Sender.Tick(0.0f);
		TestEqual(TEXT("Retry after the second backoff"), Fake->GetBlockingLaunches(), 3);

		// All three blocking connects now return successfully. Only the third is current.
		Fake->bHoldBlockingWork = false;
		Fake->RunHeldWork();
		MoQFakeTest::Pump();

		TestEqual(TEXT("Abandoned attempt 1 was closed once"), Fake->GetDisconnectCalls(1), 1);
		TestEqual(TEXT("Abandoned attempt 2 was closed once"), Fake->GetDisconnectCalls(2), 1);
		TestEqual(TEXT("Nothing announced on abandoned client 1"), Fake->GetAnnounced(1).Num(), 0);
		TestEqual(TEXT("Nothing announced on abandoned client 2"), Fake->GetAnnounced(2).Num(), 0);
		TestTrue(TEXT("Current attempt announced mocap"), Fake->GetAnnounced(3).Contains(MocapNamespace));
		TestEqual(TEXT("One publisher, on the current client"), Fake->GetPublishersCreated(), 1);
		TestTrue(TEXT("Abandoned client 1 destroyed"), Fake->IsClientDestroyed(1));
		TestTrue(TEXT("Abandoned client 2 destroyed"), Fake->IsClientDestroyed(2));

		Sender.Stop();
	}
	MoQFakeTest::Pump();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQSenderConnectErrorWithoutCallbackTest, "Open3DBroadcast.Transport.MoQ.Sender.ConnectErrorWithoutCallbackRetries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQSenderConnectErrorWithoutCallbackTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("moq_connect failed"), EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedError(TEXT("MoQ session state"), EAutomationExpectedMessageFlags::Contains, 2);

	// moq-ffi returns some errors (bad URL scheme, null arguments) without a FAILED callback.
	// The in-flight flag must still clear so the sender retries (TRF-11).
	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	Fake->ConnectBehavior = FMoQFakeFfi::EConnectBehavior::FailWithoutCallback;
	MoQFakeTest::FManualClock Clock;

	{
		FO3DMoQSender Sender(Fake->MakeApi(), Clock.AsFunction(), /*JitterSeed=*/3);
		TestTrue(TEXT("Initialize"), Sender.Initialize(MakeFakeConfig()));
		TestTrue(TEXT("Start"), Sender.Start());
		MoQFakeTest::Pump(); // FAILED synthesised by the session

		Clock.Advance(1.0); // 1 failure: backoff at most 1.0 s
		Sender.Tick(0.0f);
		TestEqual(TEXT("Retried after an error with no callback"), Fake->GetBlockingLaunches(), 2);
		MoQFakeTest::Pump();

		Fake->ConnectBehavior = FMoQFakeFfi::EConnectBehavior::Succeed;
		Clock.Advance(2.0); // 2 failures: backoff at most 2.0 s
		Sender.Tick(0.0f);
		TestEqual(TEXT("Retried again"), Fake->GetBlockingLaunches(), 3);
		MoQFakeTest::Pump();
		TestEqual(TEXT("Connected on the third attempt"), Fake->GetPublishersCreated(), 1);

		Sender.Stop();
	}
	MoQFakeTest::Pump();

	TestEqual(TEXT("Every FFI message string was freed exactly once"), Fake->GetStringsFreed(), Fake->GetStringsAllocated());
	TestEqual(TEXT("Two error messages were produced"), Fake->GetStringsAllocated(), 2);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRF-39: moq_last_error is thread-local and read only right after a failing call
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQLastErrorHandlingTest, "Open3DBroadcast.Transport.MoQ.Session.LastErrorHandling", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQLastErrorHandlingTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("moq_connect failed"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedError(TEXT("MoQ session state"), EAutomationExpectedMessageFlags::Contains, 2);

	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	Fake->ConnectBehavior = FMoQFakeFfi::EConnectBehavior::FailWithoutCallback;
	const uint32 ThisThread = FPlatformTLS::GetCurrentThreadId();

	// Declared before the session so they outlive its delegate.
	int32 FailedCount = 0;
	int32 ConnectedCount = 0;

	TSharedRef<FMoQSessionWrapper, ESPMode::ThreadSafe> Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>(Fake->MakeApi());
	TestTrue(TEXT("Initialize"), Session->Initialize(TEXT("https://fake.relay.invalid:443")).IsOk());

	Session->OnConnectionStateChanged().AddLambda([&FailedCount, &ConnectedCount](MoqConnectionState State)
	{
		FailedCount += (State == MOQ_STATE_FAILED) ? 1 : 0;
		ConnectedCount += (State == MOQ_STATE_CONNECTED) ? 1 : 0;
	});

	// The blocking connect runs inline here, so the failing call and the error read share a thread.
	TestTrue(TEXT("Connect starts"), Session->Connect().IsOk());
	MoQFakeTest::Pump();
	TestEqual(TEXT("A terminal FAILED was reported"), FailedCount, 1);

	const FString ConnectError = Session->GetLastConnectError();
	TestTrue(TEXT("Error keeps the result message"), ConnectError.Contains(TEXT("fake invalid argument")));
	TestTrue(TEXT("Error keeps the thread-local detail"), ConnectError.Contains(TEXT("fake: invalid url scheme")));
	TestEqual(TEXT("Result message freed once"), Fake->GetStringsFreed(), 1);
	TestEqual(TEXT("moq_last_error read once, after the failing call"), Fake->GetLastErrorCalls(), 1);
	TestTrue(TEXT("...on the thread that failed"), Fake->GetLastErrorCallerThreads().Num() == 1 && Fake->GetLastErrorCallerThreads()[0] == ThisThread);

	// A connected session whose subscribe fails reads the error on the calling thread too.
	Fake->ConnectBehavior = FMoQFakeFfi::EConnectBehavior::Succeed;
	TestTrue(TEXT("Reconnect starts"), Session->Connect().IsOk());
	MoQFakeTest::Pump();
	TestEqual(TEXT("Connected"), ConnectedCount, 1);
	TestEqual(TEXT("Success reads no error"), Fake->GetLastErrorCalls(), 1);

	Fake->bSubscribeFails = true;
	FMoQSubscriptionConfig SubscribeConfig;
	SubscribeConfig.Namespace = MocapNamespace;
	SubscribeConfig.TrackName = TEXT("actor");
	SubscribeConfig.OnData = [](const TArray64<uint8>&) {};
	TSharedPtr<FMoQSubscriberHandle> Subscriber;
	const FMoQResult SubscribeResult = Session->Subscribe(SubscribeConfig, Subscriber);
	TestFalse(TEXT("Subscribe failed"), SubscribeResult.IsOk());
	TestTrue(TEXT("Subscribe error carries moq_last_error"), SubscribeResult.Message.Contains(TEXT("fake: track not announced")));
	TestEqual(TEXT("moq_last_error read for the null subscriber"), Fake->GetLastErrorCalls(), 2);
	TestTrue(TEXT("...on the calling thread"), Fake->GetLastErrorCallerThreads().Last() == ThisThread);

	// An unexpected state callback must not consult moq_last_error: another thread owns it.
	TestTrue(TEXT("Fake fired DISCONNECTED"), Fake->FireConnectionState(2, MOQ_STATE_DISCONNECTED));
	MoQFakeTest::Pump();
	TestEqual(TEXT("State callback read no thread-local error"), Fake->GetLastErrorCalls(), 2);
	TestFalse(TEXT("Session reads disconnected"), Session->IsConnected());

	Session->Disconnect();
	MoQFakeTest::Pump();
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRF-20: subscribe retries back off
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverSubscribeBackoffTest, "Open3DBroadcast.Transport.MoQ.Receiver.SubscribeRetryBackoff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverSubscribeBackoffTest::RunTest(const FString& Parameters)
{
	// Logged once; later failures fall inside the 5 s log rate limit of the manual clock.
	AddExpectedError(TEXT("Failed to subscribe to mocap track"), EAutomationExpectedMessageFlags::Contains, 1);

	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	Fake->bSubscribeFails = true; // publisher has not announced yet
	MoQFakeTest::FManualClock Clock;

	{
		FO3DMoQReceiver Receiver(Fake->MakeApi(), Clock.AsFunction(), /*JitterSeed=*/11);
		TestTrue(TEXT("Initialize"), Receiver.Initialize(MakeFakeConfig()));
		TestTrue(TEXT("Start"), Receiver.Start());
		MoQFakeTest::Pump(); // CONNECTED -> immediate subscribe, fails, next try in [0.75, 1.0] s
		TestEqual(TEXT("Immediate subscribe on connect"), Fake->GetSubscribeCalls(), 1);

		for (int32 Index = 0; Index < 10; ++Index)
		{
			Receiver.Poll();
		}
		TestEqual(TEXT("No subscribe per Poll"), Fake->GetSubscribeCalls(), 1);

		Clock.Advance(0.5);
		Receiver.Poll();
		TestEqual(TEXT("No retry inside the first backoff"), Fake->GetSubscribeCalls(), 1);

		Clock.Advance(0.5);
		Receiver.Poll();
		TestEqual(TEXT("Retry after the first backoff"), Fake->GetSubscribeCalls(), 2);

		Clock.Advance(1.25); // 2 failures: [1.5, 2.0] s
		Receiver.Poll();
		TestEqual(TEXT("No retry inside the second backoff"), Fake->GetSubscribeCalls(), 2);

		Clock.Advance(0.75);
		Receiver.Poll();
		TestEqual(TEXT("Retry after the second backoff"), Fake->GetSubscribeCalls(), 3);

		Fake->bSubscribeFails = false; // publisher appears
		Clock.Advance(4.0); // 3 failures: [3.0, 4.0] s
		Receiver.Poll();
		TestEqual(TEXT("Retry after the third backoff succeeds"), Fake->GetSubscribeCalls(), 4);

		Clock.Advance(30.0);
		for (int32 Index = 0; Index < 5; ++Index)
		{
			Receiver.Poll();
		}
		TestEqual(TEXT("No subscribe once subscribed"), Fake->GetSubscribeCalls(), 4);

		Receiver.Stop();
	}
	MoQFakeTest::Pump();
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRF-37: the receiver decodes with the codec written in the frame
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQReceiverCodecFromFrameTest, "Open3DBroadcast.Transport.MoQ.Receiver.AudioCodecFromFrame", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQReceiverCodecFromFrameTest::RunTest(const FString& Parameters)
{
	O3DS::FAudioFrameMeta Meta;
	Meta.SourceGuid = FGuid::NewGuid();
	Meta.StreamLabel = TEXT("actor");
	Meta.SubjectName = TEXT("actor");
	Meta.NumChannels = 1;
	Meta.SampleRate = 48000;

	TArray<uint8> PcmBytes;
	PcmBytes.Init(0x11, 8); // four PCM16 samples
	TArray<uint8> PcmFrame;
	TestTrue(TEXT("Serialize PCM16 frame"), O3DAudio::SerializeEncodedAudioFrame(O3DS::EUnifiedCodec::PCM16, Meta, PcmBytes.GetData(), PcmBytes.Num(), PcmFrame));

	TArray<uint8> OpusBytes;
	OpusBytes.Init(0x22, 12); // opaque bytes; only the header is inspected here
	TArray<uint8> OpusFrame;
	TestTrue(TEXT("Serialize Opus-labelled frame"), O3DAudio::SerializeEncodedAudioFrame(O3DS::EUnifiedCodec::Opus, Meta, OpusBytes.GetData(), OpusBytes.Num(), OpusFrame));

	O3DS::EUnifiedCodec Codec = O3DS::EUnifiedCodec::O3DS;
	TestTrue(TEXT("PCM16 frame recognised"), MoQHelpers::TryGetAudioCodecFromFrame(PcmFrame.GetData(), PcmFrame.Num(), Codec));
	TestTrue(TEXT("...as PCM16"), Codec == O3DS::EUnifiedCodec::PCM16);
	TestTrue(TEXT("Opus frame recognised"), MoQHelpers::TryGetAudioCodecFromFrame(OpusFrame.GetData(), OpusFrame.Num(), Codec));
	TestTrue(TEXT("...as Opus"), Codec == O3DS::EUnifiedCodec::Opus);
	const uint8 Garbage[3] = {9, 9, 9};
	TestFalse(TEXT("Unknown header rejected"), MoQHelpers::TryGetAudioCodecFromFrame(Garbage, 3, Codec));
	TestFalse(TEXT("Empty payload rejected"), MoQHelpers::TryGetAudioCodecFromFrame(nullptr, 0, Codec));

	// End to end: the receiver's local config says Opus, the sender sent PCM16. Before WP-S8
	// the receiver decoded with its own config and dropped every frame.
	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	MoQFakeTest::FManualClock Clock;
	FO3DTransportConfig Config = MakeFakeConfig();
	Config.Audio.Codec = TEXT("Opus");
	TSharedRef<FRecordingAudioSink, ESPMode::ThreadSafe> Sink = MakeShared<FRecordingAudioSink, ESPMode::ThreadSafe>();

	{
		FO3DMoQReceiver Receiver(Fake->MakeApi(), Clock.AsFunction(), /*JitterSeed=*/5);
		TestTrue(TEXT("Initialize"), Receiver.Initialize(Config));
		Receiver.SetAudioSink(Sink, Config.Audio);
		TestTrue(TEXT("Start"), Receiver.Start());
		MoQFakeTest::Pump(); // CONNECTED -> mocap and audio subscriptions

		TestTrue(TEXT("Audio frame delivered to the audio subscription"), Fake->DeliverData(AudioNamespace, PcmFrame));
		MoQFakeTest::Pump(); // data callback -> receive queue
		Receiver.Poll();

		TestEqual(TEXT("Sink got the frame"), Sink->Frames, 1);
		TestEqual(TEXT("Sink got the PCM bytes"), Sink->LastBytes, PcmBytes.Num());
		TestEqual(TEXT("Meta survived"), Sink->LastLabel, Meta.StreamLabel);

		Receiver.Stop();
	}
	MoQFakeTest::Pump();
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TRF-13: no work is accepted after the dispatcher shuts down
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQDispatcherShutdownTest, "Open3DBroadcast.Transport.MoQ.Dispatcher.NoWorkAfterShutdown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQDispatcherShutdownTest::RunTest(const FString& Parameters)
{
	FMoQAsyncDispatcher& Dispatcher = FMoQAsyncDispatcher::Get();
	const bool bWasAccepting = Dispatcher.IsAcceptingTasks();
	Dispatcher.Initialize();
	Dispatcher.DrainOnGameThread();

	int32 Ran = 0;
	TestTrue(TEXT("Queued before shutdown"), Dispatcher.EnqueueGameThreadTask([&Ran]() { ++Ran; }));
	Dispatcher.Shutdown();
	TestEqual(TEXT("Queued work is discarded, not run, at shutdown"), Ran, 0);
	TestFalse(TEXT("Rejected after shutdown"), Dispatcher.EnqueueGameThreadTask([&Ran]() { ++Ran; }));
	TestFalse(TEXT("Not restarted lazily"), Dispatcher.IsAcceptingTasks());
	TestEqual(TEXT("Nothing to drain"), Dispatcher.DrainOnGameThread(), 0);

	// Restore the module's state for later tests.
	Dispatcher.Initialize();
	TestTrue(TEXT("Accepting again after Initialize"), Dispatcher.EnqueueGameThreadTask([&Ran]() { ++Ran; }));
	Dispatcher.DrainOnGameThread();
	TestEqual(TEXT("Runs after re-initialize"), Ran, 1);
	if (!bWasAccepting)
	{
		Dispatcher.Shutdown();
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// WP-S5 stress extended through Start() (TRF-9 publisher handles under the worker)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQLifetimeStartStressTest, "Open3DBroadcast.Transport.MoQ.Lifetime.StartStopWithAudio", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMoQLifetimeStartStressTest::RunTest(const FString& Parameters)
{
	TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	const FMoQFfiApiRef Api = Fake->MakeApi();
	const FO3DTransportConfig Config = MakeFakeConfig();

	O3DLifetimeTest::FFakeAudioThread AudioThread;
	const float Probe[4] = {0.1f, -0.1f, 0.2f, -0.2f};
	const uint8 MocapBytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

	int32 StartFailures = 0;
	int32 SinksCreated = 0;
	int32 StaleSinkAccepted = 0;
	TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> StaleSink;

	for (int32 Cycle = 0; Cycle < O3DLifetimeTest::StressCycles; ++Cycle)
	{
		TSharedPtr<FO3DMoQSender> Sender = MakeShared<FO3DMoQSender>(Api, nullptr, static_cast<uint64>(Cycle));
		if (!Sender->Initialize(Config) || !Sender->Start())
		{
			++StartFailures;
			Sender->Stop();
			continue;
		}
		MoQFakeTest::Pump(); // CONNECTED -> the worker has publishers to publish on

		TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink = Sender->CreateAudioSink(Config.Audio);
		SinksCreated += Sink.IsValid() ? 1 : 0;
		AudioThread.SetSink(Sink);

		for (int32 TickIndex = 0; TickIndex < 3; ++TickIndex)
		{
			Sender->SendSerialized(MocapBytes, static_cast<int32>(UE_ARRAY_COUNT(MocapBytes)), TEXT("actor"), static_cast<double>(TickIndex));
			Sender->Tick(0.0f);
			FPlatformProcess::YieldThread();
		}

		if ((Cycle & 1) == 0)
		{
			AudioThread.SetSink(nullptr);
		}

		// Stop destroys the publishers on the game thread while the worker may be publishing.
		Sender->Stop();
		Sender.Reset();
		MoQFakeTest::Pump();

		if (Sink.IsValid() && Sink->SubmitPcm(TEXT("stale"), Probe, 4, 1, 48000, 0.0))
		{
			++StaleSinkAccepted;
		}
		AudioThread.SetSink(Sink);
		StaleSink = Sink;
	}

	AudioThread.SetSink(nullptr);
	AudioThread.StopAndJoin();
	StaleSink.Reset();
	MoQFakeTest::Pump();

	TestEqual(TEXT("Every cycle started"), StartFailures, 0);
	TestEqual(TEXT("Every cycle produced a sink"), SinksCreated, O3DLifetimeTest::StressCycles);
	TestEqual(TEXT("No sink accepts PCM after its sender stopped"), StaleSinkAccepted, 0);
	TestEqual(TEXT("Every publisher destroyed"), Fake->GetPublishersDestroyed(), Fake->GetPublishersCreated());
	TestEqual(TEXT("Mocap and audio publisher per cycle"), Fake->GetPublishersCreated(), 2 * O3DLifetimeTest::StressCycles);

	int32 LiveClients = 0;
	for (int32 ClientId = 1; ClientId <= Fake->GetClientsCreated(); ++ClientId)
	{
		LiveClients += Fake->IsClientDestroyed(ClientId) ? 0 : 1;
	}
	TestEqual(TEXT("Every client destroyed"), LiveClients, 0);
	AddInfo(FString::Printf(TEXT("Worker published %d payloads; fake audio thread submitted %lld buffers, %lld accepted"),
		Fake->GetPublishCalls(), AudioThread.GetSubmitted(), AudioThread.GetAccepted()));
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
