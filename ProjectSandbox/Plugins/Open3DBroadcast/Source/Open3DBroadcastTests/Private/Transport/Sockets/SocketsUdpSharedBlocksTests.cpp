// Copyright Lifelike & Believable. All Rights Reserved.

// UDP on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4c), on 127.0.0.1 with real
// sockets and ephemeral ports:
// - QueueDropsOldestUnderBackpressure: with the sender's worker paused, frames beyond twice the
//   soft cap are refused (DroppedBackpressure); when the worker resumes, the oldest frames above
//   the soft cap are dropped and the newest are sent (EO3DMocapOverflow::DropOldest).
// - AudioIndependentOfFrameQueue: with the frame budget full, audio and control are still
//   accepted and delivered.
// - StopWhileSending: ADR 0007's Verification case, Stop() while four threads call
//   SendSerialized/SendControl and a fake audio thread submits PCM, 1,000 cycles, every one with
//   a bound destination socket so the worker is sending when Stop runs.
// No fixed sleeps: every wait polls a condition against a deadline. Localhost UDP does not lose
// datagrams in practice; the delivery checks allow generous timeouts.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Testing/O3DLifetimeTestUtils.h"
#include "Testing/SocketsTesting.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "O3DUnifiedMessage.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>

namespace O3DSocketsUdpBlocksTests
{
	/** FO3DSocketsUdpSender's soft cap on waiting frames; callers are refused at twice it. */
	constexpr int32 FrameSoftCap = 4;

	FO3DTransportConfig MakeUdpConfig(bool bSender, int32 Port)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.udp");
		Config.Role = bSender ? TEXT("sender") : TEXT("receiver");
		Config.Uri = FString::Printf(TEXT("udp://127.0.0.1:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		return Config;
	}

	/** "SEQ:" + index; never a unified envelope. */
	TArray<uint8> MakeIndexedFrame(int32 Index)
	{
		TArray<uint8> Frame;
		Frame.SetNumUninitialized(64);
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

	TArray<uint8> MakeUdpControlEnvelope()
	{
		const TArray<uint8> Payload = { 9, 8, 7, 6, 5, 4, 3, 2 };
		TArray<uint8> Envelope;
		O3DS::WriteControlEnvelope(Payload, 1.0, Envelope);
		return Envelope;
	}

	template <typename TCondition>
	bool PollReceiverFor(IOpen3DReceiver& Receiver, double TimeoutSeconds, TCondition&& Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
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

	class FUdpBlocksAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& /*Meta*/, const uint8* /*Data*/, int32 /*NumBytes*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	class FUdpBlocksControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> /*Payload*/, const FString& /*StreamId*/, double /*ReceiveTimeSec*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	/** A started UDP sender and receiver pair on one ephemeral port. */
	struct FUdpBlocksPair
	{
		TSharedRef<IOpen3DSender> Sender = O3DSocketsTesting::CreateUdpSender();
		TSharedRef<IOpen3DReceiver> Receiver = O3DSocketsTesting::CreateUdpReceiver();
		TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		TSharedRef<FUdpBlocksAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FUdpBlocksAudioSink, ESPMode::ThreadSafe>();
		TSharedRef<FUdpBlocksControlSink, ESPMode::ThreadSafe> ControlSink = MakeShared<FUdpBlocksControlSink, ESPMode::ThreadSafe>();
		FO3DTransportConfig SenderConfig;

		bool Setup(FAutomationTestBase& Test)
		{
			const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
			if (!Test.TestTrue(TEXT("Loopback UDP port allocated"), Port > 0))
			{
				return false;
			}
			SenderConfig = MakeUdpConfig(true, Port);
			const FO3DTransportConfig ReceiverConfig = MakeUdpConfig(false, Port);
			if (!Test.TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(ReceiverConfig).IsOk())
				|| !Test.TestTrue(TEXT("Sender initializes"), Sender->Initialize(SenderConfig).IsOk()))
			{
				return false;
			}
			Receiver->SetConsumer(Consumer);
			Receiver->SetAudioSink(AudioSink, ReceiverConfig.Audio);
			Receiver->SetControlSink(ControlSink);
			return Test.TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk())
				&& Test.TestTrue(TEXT("Sender starts"), Sender->Start().IsOk());
		}

		~FUdpBlocksPair()
		{
			O3DSocketsTesting::UdpSenderSetWorkerPaused(*Sender, false);
			Receiver->Stop();
			Sender->Stop();
		}
	};

	/** Calls SendSerialized and SendControl in a loop on whichever sender is published. */
	class FUdpSendHammer final : public FRunnable
	{
	public:
		FUdpSendHammer()
			: Envelope(MakeUdpControlEnvelope())
			, Frame(MakeIndexedFrame(7))
		{
			Thread = FRunnableThread::Create(this, TEXT("O3D_UdpSendHammer"));
		}
		virtual ~FUdpSendHammer() override { StopAndJoin(); }

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

	/** A UDP socket bound to 127.0.0.1 that receives (and never reads) the stress test's datagrams. */
	FSocket* BindDiscardSocket(int32& OutPort)
	{
		OutPort = 0;
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return nullptr;
		}
		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bValid);
		Addr->SetPort(0);
		FSocket* Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UdpBlocksDiscard"), false);
		if (Socket && (!bValid || !Socket->Bind(*Addr)))
		{
			SocketSubsystem->DestroySocket(Socket);
			return nullptr;
		}
		if (Socket)
		{
			Socket->GetAddress(*Addr);
			OutPort = Addr->GetPort();
		}
		return Socket;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpDropOldestTest, "Open3DBroadcast.Transport.Sockets.Udp.QueueDropsOldestUnderBackpressure", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpDropOldestTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsUdpBlocksTests;
	FUdpBlocksPair Pair;
	if (!Pair.Setup(*this))
	{
		return false;
	}

	// The worker sends nothing while paused, so every frame waits in the queue.
	O3DSocketsTesting::UdpSenderSetWorkerPaused(*Pair.Sender, true);
	constexpr int32 NumSends = 20;
	TArray<int32> AcceptedIndices;
	int32 Refused = 0;
	for (int32 Index = 0; Index < NumSends; ++Index)
	{
		const TArray<uint8> Frame = MakeIndexedFrame(Index);
		const EO3DSendResult Result = Pair.Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Drop"), 0.0));
		if (Result == EO3DSendResult::Queued)
		{
			AcceptedIndices.Add(Index);
		}
		else if (Result == EO3DSendResult::DroppedBackpressure)
		{
			++Refused;
		}
	}
	// Callers are refused only at the hard cap, twice the soft cap.
	TestEqual(TEXT("Frames accepted up to the hard cap"), AcceptedIndices.Num(), 2 * FrameSoftCap);
	TestEqual(TEXT("The rest refused with DroppedBackpressure"), Refused, NumSends - 2 * FrameSoftCap);
	TestEqual(TEXT("Frames waiting"), static_cast<int32>(Pair.Sender->GetStats().PendingFrames), 2 * FrameSoftCap);

	// The worker resumes: the oldest frames above the soft cap are dropped, the newest are sent.
	O3DSocketsTesting::UdpSenderSetWorkerPaused(*Pair.Sender, false);
	TestTrue(TEXT("The newest frames arrive"), PollReceiverFor(*Pair.Receiver, 10.0, [&Pair]() { return Pair.Consumer->Num() >= FrameSoftCap; }));
	// Nothing more arrives: the dropped frames are gone.
	PollReceiverFor(*Pair.Receiver, 0.3, []() { return false; });

	const TArray<TArray<uint8>> Frames = Pair.Consumer->GetFrames();
	TestEqual(TEXT("Exactly the soft cap's worth of frames was sent"), Frames.Num(), FrameSoftCap);
	bool bNewest = Frames.Num() == FrameSoftCap && AcceptedIndices.Num() == 2 * FrameSoftCap;
	for (int32 Index = 0; bNewest && Index < Frames.Num(); ++Index)
	{
		bNewest = ReadFrameIndex(Frames[Index]) == AcceptedIndices[FrameSoftCap + Index];
	}
	TestTrue(TEXT("They are the newest accepted frames, in order (DropOldest)"), bNewest);
	const FO3DTransportStats Stats = Pair.Sender->GetStats();
	TestEqual(TEXT("Frames sent"), Stats.FramesSent, static_cast<int64>(FrameSoftCap));
	TestEqual(TEXT("Dropped: refused plus the oldest dropped"), Stats.DroppedFrames, static_cast<int64>(Refused + FrameSoftCap));
	TestEqual(TEXT("Nothing waits any more"), Stats.PendingFrames, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpAudioIndependentTest, "Open3DBroadcast.Transport.Sockets.Udp.AudioIndependentOfFrameQueue", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpAudioIndependentTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsUdpBlocksTests;
	FUdpBlocksPair Pair;
	if (!Pair.Setup(*this))
	{
		return false;
	}

	O3DSocketsTesting::UdpSenderSetWorkerPaused(*Pair.Sender, true);
	bool bRefused = false;
	for (int32 Index = 0; Index < 4 * FrameSoftCap && !bRefused; ++Index)
	{
		const TArray<uint8> Frame = MakeIndexedFrame(Index);
		bRefused = Pair.Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Fill"), 0.0)) == EO3DSendResult::DroppedBackpressure;
	}
	TestTrue(TEXT("The frame budget is full (a frame was refused)"), bRefused);

	// Audio and control have budgets of their own (ADR 0007 item 7, ADR 0011).
	const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudio = Pair.Sender->CreateAudioSink(Pair.SenderConfig.Audio);
	const float Samples[8] = { 0.1f, 0.2f, 0.3f, 0.4f, -0.1f, -0.2f, -0.3f, -0.4f };
	TestTrue(TEXT("Audio accepted while frames are refused"), SenderAudio.IsValid() && SenderAudio->SubmitPcm(TEXT("voice"), Samples, 8, 1, 48000, 1.0));
	const TArray<uint8> Envelope = MakeUdpControlEnvelope();
	TestTrue(TEXT("Control accepted while frames are refused"), Pair.Sender->SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued);

	O3DSocketsTesting::UdpSenderSetWorkerPaused(*Pair.Sender, false);
	TestTrue(TEXT("Audio, control and the newest frames arrive"), PollReceiverFor(*Pair.Receiver, 10.0, [&Pair]()
	{
		return Pair.AudioSink->Calls.load() >= 1 && Pair.ControlSink->Calls.load() >= 1 && Pair.Consumer->Num() >= FrameSoftCap;
	}));
	TestEqual(TEXT("One audio frame"), Pair.AudioSink->Calls.load(), 1);
	TestEqual(TEXT("One control payload"), Pair.ControlSink->Calls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpStopUnderLoadTest, "Open3DBroadcast.Transport.Sockets.Udp.StopWhileSending", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpStopUnderLoadTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsUdpBlocksTests;
	// 1,000 cycles, as ADR 0007's Verification case and the WP-S5 UDP stress run. Every cycle sends
	// to a bound socket, so the worker is inside SendTo while Stop joins it. Starting and stopping a
	// UDP sender is cheap (no handshake), so the full count stays short on CI.
	int32 Port = 0;
	FSocket* Discard = BindDiscardSocket(Port);
	if (!TestNotNull(TEXT("Loopback UDP destination bound"), Discard))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Discard);
	};

	TArray<TUniquePtr<FUdpSendHammer>> Hammers;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Hammers.Add(MakeUnique<FUdpSendHammer>());
	}
	O3DLifetimeTest::FFakeAudioThread AudioThread;

	int32 StartFailures = 0;
	int32 AcceptedAfterStop = 0;
	int32 SlowStops = 0;
	for (int32 Cycle = 0; Cycle < O3DLifetimeTest::StressCycles; ++Cycle)
	{
		const FO3DTransportConfig Config = MakeUdpConfig(true, Port);
		TSharedPtr<IOpen3DSender> Sender = O3DSocketsTesting::CreateUdpSender();
		if (!Sender->Initialize(Config).IsOk() || !Sender->Start().IsOk())
		{
			++StartFailures;
			continue;
		}

		AudioThread.SetSink(Sender->CreateAudioSink(Config.Audio));
		for (const TUniquePtr<FUdpSendHammer>& Hammer : Hammers)
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
			for (const TUniquePtr<FUdpSendHammer>& Hammer : Hammers)
			{
				Hammer->SetSender(nullptr);
			}
			AudioThread.SetSink(nullptr);
		}
		Sender.Reset();
	}

	int64 Calls = 0;
	for (TUniquePtr<FUdpSendHammer>& Hammer : Hammers)
	{
		Hammer->SetSender(nullptr);
		Hammer->StopAndJoin();
		Calls += Hammer->Calls.load();
	}
	AudioThread.SetSink(nullptr);
	AudioThread.StopAndJoin();

	TestEqual(TEXT("Every cycle started"), StartFailures, 0);
	TestEqual(TEXT("No send is accepted after Stop returned"), AcceptedAfterStop, 0);
	TestEqual(TEXT("Stop returned promptly every time"), SlowStops, 0);
	AddInfo(FString::Printf(TEXT("Send threads made %lld calls; the audio thread submitted %lld buffers"), Calls, AudioThread.GetSubmitted()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
