// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// FO3DQueuedSenderAudioSink and FO3DAudioPublishState (ADR 0007 item 7 and the WP-S5 addendum;
// TRB-10, TRB-11, TRB-30, TRF-1): the sink enqueues audio items on the transport's send queue,
// never after the state was closed, never into a later session, and Close() is safe while an
// audio thread is inside a submit.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "O3DAudioSerialization.h"
#include "O3DUnifiedMessage.h"
#include "Testing/O3DTransportLifetimeTestUtils.h"
#include "Transport/O3DSenderAudioSinkBase.h"
#include "Transport/O3DSendQueue.h"

namespace O3DAudioSinkBaseTests
{
	FO3DTransportAudioConfig MakeAudioConfig()
	{
		FO3DTransportAudioConfig Config;
		Config.bEnableAudio = true;
		Config.SampleRate = 48000;
		Config.NumChannels = 1;
		return Config;
	}

	TSharedRef<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe> MakeSink(const FO3DAudioPublishStateRef& State)
	{
		FO3DSinkAudioEncoder::FSettings Settings;
		Settings.Config = MakeAudioConfig();
		Settings.DefaultStreamLabel = TEXT("default_label");
		Settings.DefaultSubject = TEXT("default_subject");
		Settings.SourceGuid = FGuid::NewGuid();
		return MakeShared<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe>(State, MakeAudioConfig(), MoveTemp(Settings));
	}

	bool Submit(IO3DSenderAudioSink& Sink, const TCHAR* Label = TEXT("voice"))
	{
		const float Samples[8] = { 0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f };
		return Sink.SubmitPcm(Label, Samples, 8, 1, 48000, 2.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioSinkBaseEnqueueTest, "Open3DBroadcast.Shared.AudioSinkBase.EnqueuesAudioItems", O3DB_TEST_FLAGS)
bool FO3DAudioSinkBaseEnqueueTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioSinkBaseTests;
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue = MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>();
	const FO3DAudioPublishStateRef State = MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue);
	TestTrue(TEXT("Open returns an epoch"), State->Open() != 0);
	State->GetSubjectSlot().Set(TEXT("Hero"));
	const TSharedRef<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe> Sink = MakeSink(State);
	TestTrue(TEXT("Sink bound to the open epoch"), Sink->GetBoundEpoch() == State->GetEpoch());

	TestTrue(TEXT("Submit accepted"), Submit(*Sink));
	TestFalse(TEXT("Invalid input refused"), Sink->SubmitPcm(TEXT("voice"), nullptr, 8, 1, 48000, 0.0));
	TestEqual(TEXT("One audio item queued"), Queue->GetPendingItems(EO3DSendItemKind::Audio), 1);
	TestEqual(TEXT("No mocap or control"), Queue->GetPendingItems(EO3DSendItemKind::Mocap) + Queue->GetPendingItems(EO3DSendItemKind::Control), 0);

	FO3DSendItem Item;
	TestTrue(TEXT("Dequeued"), Queue->Dequeue(Item));
	TestTrue(TEXT("Kind is Audio"), Item.Kind == EO3DSendItemKind::Audio);
	TestEqual(TEXT("The item carries the frame's subject"), Item.Subject, FString(TEXT("Hero")));
	TestEqual(TEXT("Bytes counted on the state"), State->GetAudioBytesQueued(), static_cast<int64>(Item.Bytes.Num()));

	O3DS::FUnifiedHeader Header;
	const uint8* Payload = nullptr;
	int32 PayloadSize = 0;
	TestTrue(TEXT("UnifiedEnvelope wire format"), O3DS::ParseUnifiedMessage(Item.Bytes.GetData(), Item.Bytes.Num(), Header, Payload, PayloadSize)
		&& Header.GetKind() == O3DS::EUnifiedKind::Audio);
	O3DAudio::FEncodedAudioFrame Frame;
	TestTrue(TEXT("The payload parses"), O3DAudio::DeserializeEncodedAudioFrame(Header.GetCodec(), Payload, PayloadSize, Frame));
	TestEqual(TEXT("Label"), Frame.Meta.StreamLabel, FString(TEXT("voice")));
	TestEqual(TEXT("Subject from the slot"), Frame.Meta.SubjectName, FString(TEXT("Hero")));

	// An empty label is normalised by FO3DSenderAudioSinkBase.
	TestTrue(TEXT("Empty label accepted"), Submit(*Sink, TEXT("")));
	TestTrue(TEXT("Dequeued"), Queue->Dequeue(Item));
	TestTrue(TEXT("Parses"), O3DS::ParseUnifiedMessage(Item.Bytes.GetData(), Item.Bytes.Num(), Header, Payload, PayloadSize)
		&& O3DAudio::DeserializeEncodedAudioFrame(Header.GetCodec(), Payload, PayloadSize, Frame));
	TestEqual(TEXT("Empty label becomes audio_default"), Frame.Meta.StreamLabel, FString(TEXT("audio_default")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioSinkBasePayloadFormatTest, "Open3DBroadcast.Shared.AudioSinkBase.AudioPayloadWireFormat", O3DB_TEST_FLAGS)
bool FO3DAudioSinkBasePayloadFormatTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioSinkBaseTests;
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue = MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>();
	const FO3DAudioPublishStateRef State = MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue, EO3DAudioWireFormat::AudioPayload);
	State->Open();
	TestTrue(TEXT("Submit accepted"), Submit(*MakeSink(State)));

	FO3DSendItem Item;
	TestTrue(TEXT("Dequeued"), Queue->Dequeue(Item));
	O3DS::FUnifiedHeader Header;
	const uint8* Payload = nullptr;
	int32 PayloadSize = 0;
	TestFalse(TEXT("No envelope around a bare payload"), O3DS::ParseUnifiedMessage(Item.Bytes.GetData(), Item.Bytes.Num(), Header, Payload, PayloadSize));
	O3DS::EUnifiedCodec Codec = O3DS::EUnifiedCodec::Opus;
	O3DAudio::FEncodedAudioFrame Frame;
	TestTrue(TEXT("The bare payload parses"), O3DAudio::TryGetAudioPayloadCodec(Item.Bytes.GetData(), Item.Bytes.Num(), Codec)
		&& O3DAudio::DeserializeEncodedAudioFrame(Codec, Item.Bytes.GetData(), Item.Bytes.Num(), Frame));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioSinkBaseGateTest, "Open3DBroadcast.Shared.AudioSinkBase.LifetimeGate", O3DB_TEST_FLAGS)
bool FO3DAudioSinkBaseGateTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioSinkBaseTests;
	FO3DSendQueueLimits Limits;
	Limits.Audio.MaxItems = 2;
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue = MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>(Limits);
	const FO3DAudioPublishStateRef State = MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue);

	const TSharedRef<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe> Closed = MakeSink(State);
	TestTrue(TEXT("A sink made while closed is bound to no epoch"), Closed->GetBoundEpoch() == 0);
	State->Open();
	TestFalse(TEXT("It never accepts, even after Open"), Submit(*Closed));

	const TSharedRef<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe> First = MakeSink(State);
	TestTrue(TEXT("Accepts while open"), Submit(*First));
	TestTrue(TEXT("Accepts up to the audio limit"), Submit(*First));
	TestFalse(TEXT("A full audio kind refuses the newest frame"), Submit(*First));
	Queue->Empty();

	State->Close();
	TestFalse(TEXT("Refused after Close (Stop)"), Submit(*First));
	State->Open();
	TestFalse(TEXT("A sink of an earlier session stays dead after a restart"), Submit(*First));
	TestTrue(TEXT("A sink of the new session accepts"), Submit(*MakeSink(State)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DAudioSinkBaseStopUnderLoadTest, "Open3DBroadcast.Shared.AudioSinkBase.CloseWhileProducerMidCall", O3DB_TEST_FLAGS)
bool FO3DAudioSinkBaseStopUnderLoadTest::RunTest(const FString& Parameters)
{
	using namespace O3DAudioSinkBaseTests;
	// The WP-S5 stress, on the shared pieces: a fake audio thread keeps calling SubmitPcm while the
	// game thread closes and reopens the state 1,000 times, and drops its own references while the
	// audio thread still holds (and may be inside) the sink. Run under ASan to catch a use after free.
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue = MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>();
	O3DLifetimeTest::FFakeAudioThread AudioThread(/*InFramesPerBuffer=*/480, /*InNumChannels=*/1, /*InSampleRate=*/48000);
	int32 AcceptedAfterClose = 0;
	int32 EnqueuedAfterClose = 0;

	for (int32 Cycle = 0; Cycle < O3DLifetimeTest::StressCycles; ++Cycle)
	{
		const FO3DAudioPublishStateRef State = MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue);
		State->Open();
		TSharedPtr<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe> Sink = MakeSink(State);
		AudioThread.SetSink(Sink);
		for (int32 Spin = 0; Spin < 3; ++Spin)
		{
			FPlatformProcess::YieldThread();
		}

		// Close while the audio thread may be inside SubmitPcm: Close waits for it to leave.
		State->Close();
		const int64 EnqueuedAtClose = Queue->GetStats().Audio.Enqueued;
		AcceptedAfterClose += Submit(*Sink) ? 1 : 0;
		for (int32 Spin = 0; Spin < 3; ++Spin)
		{
			FPlatformProcess::YieldThread();
		}
		EnqueuedAfterClose += Queue->GetStats().Audio.Enqueued != EnqueuedAtClose ? 1 : 0;

		// Half the cycles leave the stale sink with the audio thread; this thread's references go.
		if ((Cycle & 1) == 0)
		{
			AudioThread.SetSink(nullptr);
		}
		Sink.Reset(); // the closed state goes with the loop scope unless the audio thread's sink holds it
		Queue->Empty();
	}
	AudioThread.SetSink(nullptr);
	AudioThread.StopAndJoin();

	TestEqual(TEXT("No submit is accepted after Close returned"), AcceptedAfterClose, 0);
	TestEqual(TEXT("Nothing reaches the queue after Close returned"), EnqueuedAfterClose, 0);
	TestTrue(TEXT("The audio thread submitted"), AudioThread.GetSubmitted() > 0);
	AddInfo(FString::Printf(TEXT("Fake audio thread submitted %lld buffers, %lld accepted"), AudioThread.GetSubmitted(), AudioThread.GetAccepted()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
