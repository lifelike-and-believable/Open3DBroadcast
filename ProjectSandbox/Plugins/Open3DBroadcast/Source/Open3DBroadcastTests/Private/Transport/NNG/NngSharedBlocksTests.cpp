// Copyright Lifelike & Believable. All Rights Reserved.

// NNG on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4d), on 127.0.0.1 with real NNG
// sockets and ephemeral ports:
// - QueueRefusesNewestUnderBackpressure: with the sender's worker paused, frames beyond nng.qmax
//   are refused (DroppedBackpressure) and nothing queued is discarded; when the worker resumes,
//   exactly the accepted frames arrive, in order (EO3DMocapOverflow::RefuseNewest). Pair mode, so
//   delivery is reliable and the check can be exact.
// - AudioIndependentOfFrameQueue: with the frame budget full, audio and control are still
//   accepted and delivered.
// - StopWhileSending: ADR 0007's Verification case, Stop() while four threads call
//   SendSerialized/SendControl and a fake audio thread submits PCM, 1,000 cycles. The senders dial
//   (push) a pull receiver that listens for the whole test, so no listener is reopened on one port
//   (a closed NNG listener can linger); every 50th cycle waits until the sender is connected, so
//   Stop also runs while frames reach a peer.
// The worker is paused through a test hook rather than by relying on a slow network (HANDOFF
// pitfall 14). No fixed sleeps: every wait polls a condition against a deadline.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG

#include "Testing/NngTesting.h"
#include "Testing/O3DLifetimeTestUtils.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeLock.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>

namespace O3DNngBlocksTests
{
	/** The smallest nng.qmax the sender accepts. */
	constexpr int32 QueueLimitBytes = 64 * 1024;
	/** Frames of this size fill QueueLimitBytes exactly. */
	constexpr int32 FrameBytes = 8 * 1024;
	constexpr int32 FramesThatFit = QueueLimitBytes / FrameBytes;
	constexpr double TimeoutSeconds = 10.0;

	FO3DTransportConfig MakeNngConfig(bool bSender, const TCHAR* Mode, const TCHAR* Role, int32 Port)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("nng");
		Config.Role = bSender ? TEXT("sender") : TEXT("receiver");
		Config.AdvancedParams.Add(TEXT("nng.mode"), Mode);
		Config.AdvancedParams.Add(TEXT("nng.role"), Role);
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		if (bSender)
		{
			Config.AdvancedParams.Add(TEXT("nng.qmax"), FString::FromInt(QueueLimitBytes));
		}
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		return Config;
	}

	/** "SEQ:" + index, then filler; never a unified envelope. */
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

	int32 ReadFrameIndex(const TArray<uint8>& Frame)
	{
		int32 Index = INDEX_NONE;
		if (Frame.Num() >= 8)
		{
			FMemory::Memcpy(&Index, Frame.GetData() + 4, sizeof(int32));
		}
		return Index;
	}

	/** The indices of the received frames that are not probes (index >= 0), in arrival order. */
	TArray<int32> ReceivedIndices(const FO3DRecordingFrameConsumer& Consumer)
	{
		TArray<int32> Indices;
		for (const TArray<uint8>& Frame : Consumer.GetFrames())
		{
			const int32 Index = ReadFrameIndex(Frame);
			if (Index >= 0)
			{
				Indices.Add(Index);
			}
		}
		return Indices;
	}

	TArray<uint8> MakeNngControlEnvelope()
	{
		const TArray<uint8> Payload = { 9, 8, 7, 6, 5, 4, 3, 2 };
		TArray<uint8> Envelope;
		O3DS::WriteControlEnvelope(Payload, 1.0, Envelope);
		return Envelope;
	}

	template <typename TCondition>
	bool PollNngReceiverFor(IOpen3DReceiver& Receiver, double Seconds, TCondition&& Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			Receiver.Poll();
			if (Condition())
			{
				return true;
			}
			FPlatformProcess::YieldThread();
		}
		return Condition();
	}

	class FNngBlocksAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& /*Meta*/, const uint8* /*Data*/, int32 /*NumBytes*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	/** Counts frames without keeping them (the stress test receives many). */
	class FNngCountingFrameConsumer final : public ISerializedFrameConsumer
	{
	public:
		virtual void SubmitFrame(const FString& /*Subject*/, TConstArrayView<uint8> /*Buffer*/, double /*TimestampSeconds*/) override
		{
			Frames.fetch_add(1);
		}
		std::atomic<int64> Frames{ 0 };
	};

	class FNngBlocksControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> /*Payload*/, const FString& /*StreamId*/, double /*ReceiveTimeSec*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	/**
	 * A connected pair-mode sender (listens) and receiver (dials) on one ephemeral port. Setup
	 * returns once a probe frame went through and nothing waits in the sender's queue.
	 */
	struct FNngBlocksPair
	{
		TSharedRef<IOpen3DSender> Sender = O3DNngTesting::CreateSender();
		TSharedRef<IOpen3DReceiver> Receiver = O3DNngTesting::CreateReceiver();
		TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		TSharedRef<FNngBlocksAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FNngBlocksAudioSink, ESPMode::ThreadSafe>();
		TSharedRef<FNngBlocksControlSink, ESPMode::ThreadSafe> ControlSink = MakeShared<FNngBlocksControlSink, ESPMode::ThreadSafe>();
		FO3DTransportConfig SenderConfig;

		bool Setup(FAutomationTestBase& Test)
		{
			const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
			if (!Test.TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
			{
				return false;
			}
			SenderConfig = MakeNngConfig(true, TEXT("pair"), TEXT("server"), Port);
			const FO3DTransportConfig ReceiverConfig = MakeNngConfig(false, TEXT("pair"), TEXT("client"), Port);
			if (!Test.TestTrue(TEXT("Sender initializes"), Sender->Initialize(SenderConfig).IsOk())
				|| !Test.TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(ReceiverConfig).IsOk()))
			{
				return false;
			}
			Receiver->SetConsumer(Consumer);
			Receiver->SetAudioSink(AudioSink, ReceiverConfig.Audio);
			Receiver->SetControlSink(ControlSink);
			// The listening side starts first.
			if (!Test.TestTrue(TEXT("Sender starts"), Sender->Start().IsOk())
				|| !Test.TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk()))
			{
				return false;
			}

			// Probe until one arrives: a pair socket drops what it cannot hand to a peer yet.
			const TArray<uint8> Probe = MakeIndexedFrame(-1, 64);
			double NextProbe = 0.0;
			const bool bConnected = PollNngReceiverFor(*Receiver, TimeoutSeconds, [this, &Probe, &NextProbe]()
			{
				const double Now = FPlatformTime::Seconds();
				if (Now >= NextProbe)
				{
					NextProbe = Now + 0.05;
					Sender->SendSerialized(FO3DSendPayload::MakeCopy(Probe.GetData(), Probe.Num(), TEXT("Probe"), Now));
				}
				return Consumer->Num() > 0;
			});
			if (!Test.TestTrue(TEXT("Sender and receiver exchange a probe frame"), bConnected))
			{
				return false;
			}
			return Test.TestTrue(TEXT("The sender's queue is empty before the test"),
				PollNngReceiverFor(*Receiver, TimeoutSeconds, [this]() { return Sender->GetStats().PendingFrames == 0; }));
		}

		~FNngBlocksPair()
		{
			O3DNngTesting::SenderSetWorkerPaused(*Sender, false);
			Receiver->Stop();
			Sender->Stop();
		}
	};

	/** Calls SendSerialized and SendControl in a loop on whichever sender is published. */
	class FNngSendHammer final : public FRunnable
	{
	public:
		FNngSendHammer()
			: Envelope(MakeNngControlEnvelope())
			, Frame(MakeIndexedFrame(7, 256))
		{
			Thread = FRunnableThread::Create(this, TEXT("O3D_NngSendHammer"));
		}
		virtual ~FNngSendHammer() override { StopAndJoin(); }

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngRefuseNewestTest, "Open3DBroadcast.Transport.NNG.QueueRefusesNewestUnderBackpressure", O3DB_TEST_FLAGS)
bool FO3DNngRefuseNewestTest::RunTest(const FString& Parameters)
{
	using namespace O3DNngBlocksTests;
	FNngBlocksPair Pair;
	if (!Pair.Setup(*this))
	{
		return false;
	}

	// The worker sends nothing while paused, so every accepted frame waits in the queue.
	O3DNngTesting::SenderSetWorkerPaused(*Pair.Sender, true);
	const FO3DTransportStats Before = Pair.Sender->GetStats();
	constexpr int32 NumSends = 20;
	TArray<int32> AcceptedIndices;
	int32 Refused = 0;
	for (int32 Index = 0; Index < NumSends; ++Index)
	{
		const TArray<uint8> Frame = MakeIndexedFrame(Index, FrameBytes);
		const EO3DSendResult Result = Pair.Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Refuse"), 0.0));
		if (Result == EO3DSendResult::Queued)
		{
			AcceptedIndices.Add(Index);
		}
		else if (Result == EO3DSendResult::DroppedBackpressure)
		{
			++Refused;
		}
	}
	// RefuseNewest: nng.qmax is a hard limit, the first frames are kept and the newest refused.
	TArray<int32> Expected;
	for (int32 Index = 0; Index < FramesThatFit; ++Index)
	{
		Expected.Add(Index);
	}
	TestTrue(TEXT("The oldest frames are accepted, up to nng.qmax"), AcceptedIndices == Expected);
	TestEqual(TEXT("The newest are refused with DroppedBackpressure"), Refused, NumSends - FramesThatFit);
	TestEqual(TEXT("Frames waiting"), static_cast<int32>(Pair.Sender->GetStats().PendingFrames), FramesThatFit);

	// The worker resumes: everything accepted is sent, nothing queued was discarded.
	O3DNngTesting::SenderSetWorkerPaused(*Pair.Sender, false);
	TestTrue(TEXT("The accepted frames arrive"), PollNngReceiverFor(*Pair.Receiver, TimeoutSeconds, [&Pair]() { return ReceivedIndices(*Pair.Consumer).Num() >= FramesThatFit; }));
	// Nothing more arrives: the refused frames were never queued.
	PollNngReceiverFor(*Pair.Receiver, 0.3, []() { return false; });

	TestTrue(TEXT("Exactly the accepted frames arrived, in order"), ReceivedIndices(*Pair.Consumer) == Expected);
	const FO3DTransportStats After = Pair.Sender->GetStats();
	TestEqual(TEXT("Frames sent"), After.FramesSent - Before.FramesSent, static_cast<int64>(FramesThatFit));
	TestEqual(TEXT("Dropped: the refused frames only"), After.DroppedFrames - Before.DroppedFrames, static_cast<int64>(Refused));
	TestEqual(TEXT("Nothing waits any more"), After.PendingFrames, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngAudioIndependentTest, "Open3DBroadcast.Transport.NNG.AudioIndependentOfFrameQueue", O3DB_TEST_FLAGS)
bool FO3DNngAudioIndependentTest::RunTest(const FString& Parameters)
{
	using namespace O3DNngBlocksTests;
	FNngBlocksPair Pair;
	if (!Pair.Setup(*this))
	{
		return false;
	}

	O3DNngTesting::SenderSetWorkerPaused(*Pair.Sender, true);
	bool bRefused = false;
	for (int32 Index = 0; Index < 4 * FramesThatFit && !bRefused; ++Index)
	{
		const TArray<uint8> Frame = MakeIndexedFrame(Index, FrameBytes);
		bRefused = Pair.Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Fill"), 0.0)) == EO3DSendResult::DroppedBackpressure;
	}
	TestTrue(TEXT("The frame budget is full (a frame was refused)"), bRefused);

	// Audio and control have budgets of their own (ADR 0007 item 7, ADR 0011). Before WP-A1 PR 4d
	// they shared nng.qmax with the frames and were refused here.
	const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudio = Pair.Sender->CreateAudioSink(Pair.SenderConfig.Audio);
	const float Samples[8] = { 0.1f, 0.2f, 0.3f, 0.4f, -0.1f, -0.2f, -0.3f, -0.4f };
	TestTrue(TEXT("Audio accepted while frames are refused"), SenderAudio.IsValid() && SenderAudio->SubmitPcm(TEXT("voice"), Samples, 8, 1, 48000, 1.0));
	const TArray<uint8> Envelope = MakeNngControlEnvelope();
	TestTrue(TEXT("Control accepted while frames are refused"), Pair.Sender->SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued);

	O3DNngTesting::SenderSetWorkerPaused(*Pair.Sender, false);
	TestTrue(TEXT("Audio, control and the queued frames arrive"), PollNngReceiverFor(*Pair.Receiver, TimeoutSeconds, [&Pair]()
	{
		return Pair.AudioSink->Calls.load() >= 1 && Pair.ControlSink->Calls.load() >= 1 && ReceivedIndices(*Pair.Consumer).Num() >= FramesThatFit;
	}));
	TestEqual(TEXT("One audio frame"), Pair.AudioSink->Calls.load(), 1);
	TestEqual(TEXT("One control payload"), Pair.ControlSink->Calls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DNngStopUnderLoadTest, "Open3DBroadcast.Transport.NNG.StopWhileSending", O3DB_TEST_FLAGS)
bool FO3DNngStopUnderLoadTest::RunTest(const FString& Parameters)
{
	using namespace O3DNngBlocksTests;
	// 1,000 cycles, as ADR 0007's Verification case and the WP-S5 NNG stress run (which also opens
	// 1,000 NNG sockets). A dialing push socket needs no handshake to Start, so most cycles stop it
	// while its dial is still pending and every send is dropped at the worker (no peer); every 50th
	// cycle waits for the connection, so Stop also joins a worker that is handing frames to NNG.
	constexpr int32 ConnectedEvery = 50;
	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
	if (!TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
	{
		return false;
	}

	const TSharedRef<IOpen3DReceiver> Receiver = O3DNngTesting::CreateReceiver();
	const TSharedRef<FNngCountingFrameConsumer> Consumer = MakeShared<FNngCountingFrameConsumer>();
	if (!TestTrue(TEXT("Pull receiver initializes"), Receiver->Initialize(MakeNngConfig(false, TEXT("pull"), TEXT("server"), Port)).IsOk()))
	{
		return false;
	}
	Receiver->SetConsumer(Consumer);
	if (!TestTrue(TEXT("Pull receiver listens"), Receiver->Start().IsOk()))
	{
		return false;
	}

	TArray<TUniquePtr<FNngSendHammer>> Hammers;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Hammers.Add(MakeUnique<FNngSendHammer>());
	}
	O3DLifetimeTest::FFakeAudioThread AudioThread;

	int32 StartFailures = 0;
	int32 AcceptedAfterStop = 0;
	int32 SlowStops = 0;
	int32 ConnectWaits = 0;
	int32 ConnectedCycles = 0;
	for (int32 Cycle = 0; Cycle < O3DLifetimeTest::StressCycles; ++Cycle)
	{
		const FO3DTransportConfig Config = MakeNngConfig(true, TEXT("push"), TEXT("client"), Port);
		TSharedPtr<IOpen3DSender> Sender = O3DNngTesting::CreateSender();
		if (!Sender->Initialize(Config).IsOk() || !Sender->Start().IsOk())
		{
			++StartFailures;
			continue;
		}

		AudioThread.SetSink(Sender->CreateAudioSink(Config.Audio));
		for (const TUniquePtr<FNngSendHammer>& Hammer : Hammers)
		{
			Hammer->SetSender(Sender);
		}
		if ((Cycle % ConnectedEvery) == 0)
		{
			++ConnectWaits;
			ConnectedCycles += PollNngReceiverFor(*Receiver, TimeoutSeconds,
				[&Sender]() { return Sender->GetConnectionState() == EO3DConnectionState::Connected && Sender->GetStats().FramesSent > 0; }) ? 1 : 0;
		}
		else
		{
			Receiver->Poll();
			FPlatformProcess::YieldThread();
		}

		const double StopStart = FPlatformTime::Seconds();
		Sender->Stop();
		SlowStops += (FPlatformTime::Seconds() - StopStart) > 2.0 ? 1 : 0;
		const uint8 Probe[4] = { 9, 9, 9, 9 };
		AcceptedAfterStop += Sender->SendSerialized(FO3DSendPayload::MakeCopy(Probe, 4, TEXT("Probe"), 0.0)) == EO3DSendResult::Queued ? 1 : 0;

		// Half the cycles drop the sender while the threads still hold it.
		if ((Cycle & 1) == 0)
		{
			for (const TUniquePtr<FNngSendHammer>& Hammer : Hammers)
			{
				Hammer->SetSender(nullptr);
			}
			AudioThread.SetSink(nullptr);
		}
		Sender.Reset();
	}

	int64 Calls = 0;
	for (TUniquePtr<FNngSendHammer>& Hammer : Hammers)
	{
		Hammer->SetSender(nullptr);
		Hammer->StopAndJoin();
		Calls += Hammer->Calls.load();
	}
	AudioThread.SetSink(nullptr);
	AudioThread.StopAndJoin();
	Receiver->Stop();

	TestEqual(TEXT("Every cycle started"), StartFailures, 0);
	TestEqual(TEXT("No send is accepted after Stop returned"), AcceptedAfterStop, 0);
	TestEqual(TEXT("Stop returned promptly every time"), SlowStops, 0);
	TestEqual(TEXT("Every waiting cycle connected and sent before Stop"), ConnectedCycles, ConnectWaits);
	AddInfo(FString::Printf(TEXT("Send threads made %lld calls; the audio thread submitted %lld buffers; the receiver got %lld frames"),
		Calls, AudioThread.GetSubmitted(), Consumer->Frames.load()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_NNG
