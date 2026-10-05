// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// MoQ on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4e). Everything runs on the fake
// moq-ffi table (MoQFakeFfi.h, ADR 0006 F2): no relay and no network, so none of these needs
// O3DB_NETWORK_TESTS. The fake routes what a publisher publishes to matching subscriptions, on the
// publishing (worker) thread, and the session wrapper hands it to the game thread as moq-ffi's
// callbacks would be.
// - QueueRefusesNewestUnderBackpressure: with the sender's worker paused, frames beyond queue_bytes
//   are refused (DroppedBackpressure) and nothing queued is discarded; when the worker resumes,
//   exactly the accepted frames reach the receiver, in order (EO3DMocapOverflow::RefuseNewest).
// - AudioIndependentOfFrameQueue: with the frame budget full, audio and control are still accepted
//   and delivered on their own tracks.
// - StopWhileSending: ADR 0007's Verification case, Stop() while four threads call
//   SendSerialized/SendControl and a fake audio thread submits PCM, 1,000 cycles, every one
//   connected (through the fake), so the worker is publishing when Stop runs.
// The worker is paused through a test hook that returns once the worker has seen the flag
// (HANDOFF pitfalls 14 and 15). No fixed sleeps.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#if O3D_WITH_TRANSPORT_MOQ

#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "O3DUnifiedMessage.h"
#include "Testing/MoQTesting.h"
#include "Testing/O3DTransportLifetimeTestUtils.h"
#include "Transport/MoQ/MoQFakeFfi.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>

namespace O3DMoQBlocksTests
{
	/** Frames of this size fill the smallest queue (MoQHelpers::kMinQueueBytes, 256 KiB) eight times. */
	constexpr int32 FrameBytes = 32 * 1024;
	constexpr double TimeoutSeconds = 10.0;

	FO3DTransportConfig MakeMoQConfig(bool bSmallQueue)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("MoQ");
		Config.Uri = TEXT("https://fake.relay.invalid:443"); // only the fake FFI ever sees it
		Config.StreamId = TEXT("wpa1blocks/actor");         // mocap/wpa1blocks, track "actor"
		if (bSmallQueue)
		{
			Config.AdvancedParams.Add(TEXT("queue_bytes"), FString::Printf(TEXT("%llu"), MoQTesting::GetBackoffLimits().MinQueueBytes));
		}
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		return Config;
	}

	int32 FramesThatFit()
	{
		return static_cast<int32>(MoQTesting::GetBackoffLimits().MinQueueBytes / static_cast<uint64>(FrameBytes));
	}

	/** "SEQ:" + index, then filler. */
	TArray<uint8> MakeIndexedFrame(int32 Index, int32 Size)
	{
		TArray<uint8> Frame;
		Frame.SetNumUninitialized(FMath::Max(Size, 8));
		Frame[0] = 'S';
		Frame[1] = 'E';
		Frame[2] = 'Q';
		Frame[3] = ':';
		FMemory::Memcpy(Frame.GetData() + 4, &Index, sizeof(int32));
		for (int32 Byte = 8; Byte < Frame.Num(); ++Byte)
		{
			Frame[Byte] = static_cast<uint8>(Index + Byte);
		}
		return Frame;
	}

	TArray<int32> ReceivedIndices(const FO3DRecordingFrameConsumer& Consumer)
	{
		TArray<int32> Indices;
		for (const TArray<uint8>& Frame : Consumer.GetFrames())
		{
			int32 Index = INDEX_NONE;
			if (Frame.Num() >= 8)
			{
				FMemory::Memcpy(&Index, Frame.GetData() + 4, sizeof(int32));
			}
			Indices.Add(Index);
		}
		return Indices;
	}

	TArray<uint8> MakeMoQControlEnvelope()
	{
		const TArray<uint8> Payload = { 9, 8, 7, 6, 5, 4, 3, 2 };
		TArray<uint8> Envelope;
		O3DS::WriteControlEnvelope(Payload, 1.0, Envelope);
		return Envelope;
	}

	/** Delivers FFI callbacks and polls the receiver until Condition holds or the time runs out. */
	template <typename TCondition>
	bool PumpMoQUntil(IOpen3DReceiver* Receiver, double Seconds, TCondition&& Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			MoQFakeTest::Pump();
			if (Receiver)
			{
				Receiver->Poll();
			}
			if (Condition())
			{
				return true;
			}
			FPlatformProcess::YieldThread();
		}
		return Condition();
	}

	class FMoQBlocksAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& /*Meta*/, const uint8* /*Data*/, int32 /*NumBytes*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	class FMoQBlocksControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> /*Payload*/, const FString& /*StreamId*/, double /*ReceiveTimeSec*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	/**
	 * A connected sender and receiver on one fake relay, the sender with the smallest queue and an
	 * audio sink. Setup returns once both are connected and subscribed.
	 */
	struct FMoQBlocksPair
	{
		TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
		TSharedPtr<IOpen3DSender> Sender;
		TSharedPtr<IOpen3DReceiver> Receiver;
		TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudio;
		TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		TSharedRef<FMoQBlocksAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FMoQBlocksAudioSink, ESPMode::ThreadSafe>();
		TSharedRef<FMoQBlocksControlSink, ESPMode::ThreadSafe> ControlSink = MakeShared<FMoQBlocksControlSink, ESPMode::ThreadSafe>();
		FO3DTransportConfig SenderConfig = MakeMoQConfig(/*bSmallQueue=*/true);

		bool Setup(FAutomationTestBase& Test)
		{
			MoQTesting::InitializeDispatcher();
			Sender = MoQTesting::CreateSenderForTest(Fake->MakeApi(), nullptr, 21);
			Receiver = MoQTesting::CreateReceiverForTest(Fake->MakeApi(), nullptr, 22);
			const FO3DTransportConfig ReceiverConfig = MakeMoQConfig(/*bSmallQueue=*/false);
			if (!Test.TestTrue(TEXT("Sender initializes"), Sender->Initialize(SenderConfig).IsOk())
				|| !Test.TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(ReceiverConfig).IsOk()))
			{
				return false;
			}
			Receiver->SetConsumer(Consumer);
			Receiver->SetAudioSink(AudioSink, ReceiverConfig.Audio);
			Receiver->SetControlSink(ControlSink);

			// The sender announces first (mocap and control on connect, audio with its sink).
			if (!Test.TestTrue(TEXT("Sender starts"), Sender->Start().IsOk()))
			{
				return false;
			}
			MoQFakeTest::Pump();
			SenderAudio = Sender->CreateAudioSink(SenderConfig.Audio);
			if (!Test.TestTrue(TEXT("Sender audio sink created"), SenderAudio.IsValid())
				|| !Test.TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk()))
			{
				return false;
			}
			return Test.TestTrue(TEXT("Both ends connected"), PumpMoQUntil(Receiver.Get(), TimeoutSeconds, [this]()
			{
				return Sender->GetConnectionState() == EO3DConnectionState::Connected && Receiver->GetConnectionState() == EO3DConnectionState::Connected;
			}));
		}

		~FMoQBlocksPair()
		{
			if (Sender.IsValid())
			{
				MoQTesting::SenderSetWorkerPaused(*Sender, false);
				Sender->Stop();
			}
			if (Receiver.IsValid())
			{
				Receiver->Stop();
			}
			SenderAudio.Reset();
			Sender.Reset();
			Receiver.Reset();
			MoQFakeTest::Pump();
		}
	};

	/** Calls SendSerialized and SendControl in a loop on whichever sender is published. */
	class FMoQSendHammer final : public FRunnable
	{
	public:
		FMoQSendHammer()
			: Envelope(MakeMoQControlEnvelope())
			, Frame(MakeIndexedFrame(7, 256))
		{
			Thread = FRunnableThread::Create(this, TEXT("O3D_MoQSendHammer"));
		}
		virtual ~FMoQSendHammer() override { StopAndJoin(); }

		void SetSender(const TSharedPtr<IOpen3DSender>& InSender)
		{
			FScopeLock Lock(&Mutex);
			Sender = InSender;
		}

		void StopAndJoin()
		{
			bStop.store(true);
			if (Thread)
			{
				Thread->WaitForCompletion();
				delete Thread;
				Thread = nullptr;
			}
		}

		virtual uint32 Run() override
		{
			int64 Index = 0;
			while (!bStop.load())
			{
				TSharedPtr<IOpen3DSender> Local;
				{
					FScopeLock Lock(&Mutex);
					Local = Sender;
				}
				if (!Local.IsValid())
				{
					FPlatformProcess::YieldThread();
					continue;
				}
				if ((++Index % 8) == 0)
				{
					Local->SendControl(Envelope.GetData(), Envelope.Num());
				}
				else
				{
					Local->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Hammer"), 0.0));
				}
				Calls.fetch_add(1);
			}
			return 0;
		}

		std::atomic<int64> Calls{ 0 };

	private:
		const TArray<uint8> Envelope;
		const TArray<uint8> Frame;
		FCriticalSection Mutex;
		TSharedPtr<IOpen3DSender> Sender;
		std::atomic<bool> bStop{ false };
		FRunnableThread* Thread = nullptr;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DMoQRefuseNewestTest, "Open3DBroadcast.Transport.MoQ.QueueRefusesNewestUnderBackpressure", O3DB_TEST_FLAGS)
bool FO3DMoQRefuseNewestTest::RunTest(const FString& Parameters)
{
	using namespace O3DMoQBlocksTests;
	// The first refusal is logged as a warning (rate limited to one per 2 s per sender).
	AddExpectedError(TEXT("MoQ sender queue overflow"), EAutomationExpectedMessageFlags::Contains, 1);

	FMoQBlocksPair Pair;
	if (!Pair.Setup(*this))
	{
		return false;
	}

	// The worker publishes nothing while paused, so every accepted frame waits in the queue.
	MoQTesting::SenderSetWorkerPaused(*Pair.Sender, true);
	const FO3DTransportStats Before = Pair.Sender->GetStats();
	const int32 Fit = FramesThatFit();
	constexpr int32 NumSends = 20;
	TArray<int32> AcceptedIndices;
	int32 Refused = 0;
	for (int32 Index = 0; Index < NumSends; ++Index)
	{
		const TArray<uint8> Frame = MakeIndexedFrame(Index, FrameBytes);
		const EO3DSendResult Result = Pair.Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Refuse"), FPlatformTime::Seconds()));
		if (Result == EO3DSendResult::Queued)
		{
			AcceptedIndices.Add(Index);
		}
		else if (Result == EO3DSendResult::DroppedBackpressure)
		{
			++Refused;
		}
	}
	// RefuseNewest: queue_bytes is a hard limit, the first frames are kept and the newest refused.
	TArray<int32> Expected;
	for (int32 Index = 0; Index < Fit; ++Index)
	{
		Expected.Add(Index);
	}
	TestTrue(TEXT("The oldest frames are accepted, up to queue_bytes"), AcceptedIndices == Expected);
	TestEqual(TEXT("The newest are refused with DroppedBackpressure"), Refused, NumSends - Fit);
	TestEqual(TEXT("Frames waiting"), static_cast<int32>(Pair.Sender->GetStats().PendingFrames), Fit);

	// The worker resumes: everything accepted is published, nothing queued was discarded.
	MoQTesting::SenderSetWorkerPaused(*Pair.Sender, false);
	TestTrue(TEXT("The accepted frames arrive"), PumpMoQUntil(Pair.Receiver.Get(), TimeoutSeconds, [&Pair, Fit]() { return Pair.Consumer->Num() >= Fit; }));
	// Nothing more arrives: the refused frames were never queued.
	PumpMoQUntil(Pair.Receiver.Get(), 0.3, []() { return false; });

	TestTrue(TEXT("Exactly the accepted frames arrived, in order"), ReceivedIndices(*Pair.Consumer) == Expected);
	const FO3DTransportStats After = Pair.Sender->GetStats();
	TestEqual(TEXT("Frames sent"), After.FramesSent - Before.FramesSent, static_cast<int64>(Fit));
	TestEqual(TEXT("Dropped: the refused frames only"), After.DroppedFrames - Before.DroppedFrames, static_cast<int64>(Refused));
	TestEqual(TEXT("Nothing waits any more"), After.PendingFrames, static_cast<int64>(0));
	TestEqual(TEXT("The receiver counted each frame once"), Pair.Receiver->GetStats().FramesReceived, static_cast<int64>(Fit));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DMoQAudioIndependentTest, "Open3DBroadcast.Transport.MoQ.AudioIndependentOfFrameQueue", O3DB_TEST_FLAGS)
bool FO3DMoQAudioIndependentTest::RunTest(const FString& Parameters)
{
	using namespace O3DMoQBlocksTests;
	AddExpectedError(TEXT("MoQ sender queue overflow"), EAutomationExpectedMessageFlags::Contains, 1);

	FMoQBlocksPair Pair;
	if (!Pair.Setup(*this))
	{
		return false;
	}

	MoQTesting::SenderSetWorkerPaused(*Pair.Sender, true);
	bool bRefused = false;
	for (int32 Index = 0; Index < 4 * FramesThatFit() && !bRefused; ++Index)
	{
		const TArray<uint8> Frame = MakeIndexedFrame(Index, FrameBytes);
		bRefused = Pair.Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Fill"), FPlatformTime::Seconds())) == EO3DSendResult::DroppedBackpressure;
	}
	TestTrue(TEXT("The frame budget is full (a frame was refused)"), bRefused);

	// Audio and control have budgets of their own (ADR 0007 item 7, ADR 0011). Before WP-A1 PR 4e
	// control shared queue_bytes with the frames and was refused here.
	const float Samples[8] = { 0.1f, 0.2f, 0.3f, 0.4f, -0.1f, -0.2f, -0.3f, -0.4f };
	TestTrue(TEXT("Audio accepted while frames are refused"), Pair.SenderAudio->SubmitPcm(TEXT("voice"), Samples, 8, 1, 48000, 1.0));
	const TArray<uint8> Envelope = MakeMoQControlEnvelope();
	TestTrue(TEXT("Control accepted while frames are refused"), Pair.Sender->SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued);

	MoQTesting::SenderSetWorkerPaused(*Pair.Sender, false);
	TestTrue(TEXT("Audio, control and the queued frames arrive"), PumpMoQUntil(Pair.Receiver.Get(), TimeoutSeconds, [&Pair]()
	{
		return Pair.AudioSink->Calls.load() >= 1 && Pair.ControlSink->Calls.load() >= 1 && Pair.Consumer->Num() >= FramesThatFit();
	}));
	TestEqual(TEXT("One audio frame"), Pair.AudioSink->Calls.load(), 1);
	TestEqual(TEXT("One control payload"), Pair.ControlSink->Calls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DMoQStopUnderLoadTest, "Open3DBroadcast.Transport.MoQ.StopWhileSending", O3DB_TEST_FLAGS)
bool FO3DMoQStopUnderLoadTest::RunTest(const FString& Parameters)
{
	using namespace O3DMoQBlocksTests;
	// 1,000 cycles, as ADR 0007's Verification case and the WP-S5 MoQ stress run. The fake FFI
	// connects inline, so every cycle is connected and its worker publishes while Stop joins it.
	MoQTesting::InitializeDispatcher();
	const TSharedRef<FMoQFakeFfi, ESPMode::ThreadSafe> Fake = FMoQFakeFfi::Create();
	Fake->bRoutePublishedData = false; // no receiver here; publishing is what is under test
	const FMoQFfiApiRef Api = Fake->MakeApi();
	const FO3DTransportConfig Config = MakeMoQConfig(/*bSmallQueue=*/false);

	TArray<TUniquePtr<FMoQSendHammer>> Hammers;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Hammers.Add(MakeUnique<FMoQSendHammer>());
	}
	O3DLifetimeTest::FFakeAudioThread AudioThread;

	int32 StartFailures = 0;
	int32 NotConnected = 0;
	int32 AcceptedAfterStop = 0;
	int32 SlowStops = 0;
	for (int32 Cycle = 0; Cycle < O3DLifetimeTest::StressCycles; ++Cycle)
	{
		TSharedPtr<IOpen3DSender> Sender = MoQTesting::CreateSenderForTest(Api, nullptr, static_cast<uint64>(Cycle) + 1000);
		if (!Sender->Initialize(Config).IsOk() || !Sender->Start().IsOk())
		{
			++StartFailures;
			Sender->Stop();
			continue;
		}
		MoQFakeTest::Pump(); // CONNECTED -> publishers
		NotConnected += Sender->GetConnectionState() == EO3DConnectionState::Connected ? 0 : 1;

		AudioThread.SetSink(Sender->CreateAudioSink(Config.Audio));
		for (const TUniquePtr<FMoQSendHammer>& Hammer : Hammers)
		{
			Hammer->SetSender(Sender);
		}
		FPlatformProcess::YieldThread();
		FPlatformProcess::YieldThread();

		const double StopStart = FPlatformTime::Seconds();
		Sender->Stop();
		SlowStops += (FPlatformTime::Seconds() - StopStart) > 2.0 ? 1 : 0;
		const uint8 Probe[4] = { 9, 9, 9, 9 };
		AcceptedAfterStop += Sender->SendSerialized(FO3DSendPayload::MakeCopy(Probe, 4, TEXT("Probe"), 0.0)) == EO3DSendResult::Queued ? 1 : 0;

		// Half the cycles drop the sender while the threads still hold it.
		if ((Cycle & 1) == 0)
		{
			for (const TUniquePtr<FMoQSendHammer>& Hammer : Hammers)
			{
				Hammer->SetSender(nullptr);
			}
			AudioThread.SetSink(nullptr);
		}
		Sender.Reset();
		MoQFakeTest::Pump();
	}

	int64 Calls = 0;
	for (TUniquePtr<FMoQSendHammer>& Hammer : Hammers)
	{
		Hammer->SetSender(nullptr);
		Hammer->StopAndJoin();
		Calls += Hammer->Calls.load();
	}
	AudioThread.SetSink(nullptr);
	AudioThread.StopAndJoin();
	MoQFakeTest::Pump();

	TestEqual(TEXT("Every cycle started"), StartFailures, 0);
	TestEqual(TEXT("Every cycle connected before Stop"), NotConnected, 0);
	TestEqual(TEXT("No send is accepted after Stop returned"), AcceptedAfterStop, 0);
	TestEqual(TEXT("Stop returned promptly every time"), SlowStops, 0);
	TestEqual(TEXT("Every publisher destroyed"), Fake->GetPublishersDestroyed(), Fake->GetPublishersCreated());
	AddInfo(FString::Printf(TEXT("Send threads made %lld calls; the worker published %d payloads; the audio thread submitted %lld buffers"),
		Calls, Fake->GetPublishCalls(), AudioThread.GetSubmitted()));
	return true;
}

#endif // O3D_WITH_TRANSPORT_MOQ

#endif // WITH_DEV_AUTOMATION_TESTS
