// Copyright Lifelike & Believable. All Rights Reserved.

// TCP on the shared transport blocks (ADR 0007 item 7, WP-A1 PR 4b), on 127.0.0.1 with real
// sockets and ephemeral ports:
// - AudioIndependentOfFrameQueue: with the frame budget full (a receiver that does not poll),
//   audio and control are still accepted, and everything arrives once the receiver polls.
// - ReceiverBacksOffWithoutSender: the receiver's worker connects, backs off and reconnects
//   without Poll being called, and a connection that carries data resets the backoff.
// - StopWhileSending: ADR 0007's Verification case, Stop() while four threads call
//   SendSerialized/SendControl and a fake audio thread submits PCM, 1,000 cycles; every 20th
//   cycle has a connected client, so frames are in the queue and on the worker when Stop runs.
// No fixed sleeps: every wait polls a condition against a deadline.

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
#include "Misc/ScopeLock.h"
#include "O3DUnifiedMessage.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportTypes.h"

#include <atomic>

namespace O3DSocketsTcpBlocksTests
{
	int32 FindFreeTcpPort()
	{
		return O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
	}

	FO3DTransportConfig MakeTcpConfig(bool bSender, int32 Port, const TMap<FString, FString>& Extra)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("TCP");
		Config.Role = bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver;
		Config.Uri = FString::Printf(TEXT("tcp://127.0.0.1:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(bSender ? TEXT("bind") : TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		Config.Audio.bEnableAudio = true;
		Config.Audio.SampleRate = 48000;
		Config.Audio.NumChannels = 1;
		for (const TPair<FString, FString>& Pair : Extra)
		{
			Config.AdvancedParams.Add(Pair.Key, Pair.Value);
		}
		return Config;
	}

	template <typename TCondition>
	bool WaitFor(double TimeoutSeconds, TCondition&& Condition)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if (Condition())
			{
				return true;
			}
			FPlatformProcess::YieldThread();
		}
		return Condition();
	}

	template <typename TCondition>
	bool PollFor(IOpen3DReceiver& Receiver, double TimeoutSeconds, TCondition&& Condition)
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

	class FTcpBlocksAudioSink final : public IO3DReceiverAudioSink
	{
	public:
		virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& /*Meta*/, const uint8* /*Data*/, int32 /*NumBytes*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	class FTcpBlocksControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> /*Payload*/, const FString& /*StreamId*/, double /*ReceiveTimeSec*/) override
		{
			Calls.fetch_add(1);
		}
		std::atomic<int32> Calls{ 0 };
	};

	TArray<uint8> MakeControlEnvelope()
	{
		const TArray<uint8> Payload = { 1, 2, 3, 4, 5, 6, 7, 8 };
		TArray<uint8> Envelope;
		O3DS::WriteControlEnvelope(Payload, 1.0, Envelope);
		return Envelope;
	}

	/** Calls SendSerialized and SendControl in a loop on whichever sender is published. */
	class FTcpSendHammer final : public FRunnable
	{
	public:
		FTcpSendHammer()
			: Envelope(MakeControlEnvelope())
		{
			Frame.SetNumUninitialized(512);
			for (int32 Index = 0; Index < Frame.Num(); ++Index)
			{
				Frame[Index] = static_cast<uint8>(Index * 7);
			}
			Thread = FRunnableThread::Create(this, TEXT("O3D_TcpSendHammer"));
		}
		virtual ~FTcpSendHammer() override { StopAndJoin(); }

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
		TArray<uint8> Frame;
		const TArray<uint8> Envelope;
		FCriticalSection Mutex;
		TSharedPtr<IOpen3DSender> Sender;
		std::atomic<bool> bStop{ false };
		FRunnableThread* Thread = nullptr;
	};

	/** A plain client socket that connects to 127.0.0.1:Port and never reads. */
	FSocket* ConnectRawClient(int32 Port)
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			return nullptr;
		}
		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bValid);
		Addr->SetPort(Port);
		FSocket* Client = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TcpBlocksTestClient"), false);
		if (Client && (!bValid || !Client->Connect(*Addr)))
		{
			SocketSubsystem->DestroySocket(Client);
			Client = nullptr;
		}
		return Client;
	}

	void DestroyRawClient(FSocket*& Client)
	{
		if (Client)
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
			Client = nullptr;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpAudioIndependentTest, "Open3DBroadcast.Transport.Sockets.Tcp.AudioIndependentOfFrameQueue", O3DB_TEST_FLAGS)
bool FO3DSocketsTcpAudioIndependentTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsTcpBlocksTests;
	const int32 Port = FindFreeTcpPort();
	if (!TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
	{
		return false;
	}

	TMap<FString, FString> SenderOptions;
	SenderOptions.Add(TEXT("tcp.maxqueue"), TEXT("65536")); // the smallest frame budget
	SenderOptions.Add(TEXT("tcp.maxqueueage"), TEXT("0"));
	SenderOptions.Add(TEXT("tcp.stalltimeout"), TEXT("60000"));
	TMap<FString, FString> ReceiverOptions;
	ReceiverOptions.Add(TEXT("tcp.timeout"), TEXT("60"));
	const FO3DTransportConfig SenderConfig = MakeTcpConfig(true, Port, SenderOptions);

	const TSharedRef<IOpen3DSender> Sender = O3DSocketsTesting::CreateTcpSender();
	const TSharedRef<IOpen3DReceiver> Receiver = O3DSocketsTesting::CreateTcpReceiver();
	const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
	const TSharedRef<FTcpBlocksAudioSink, ESPMode::ThreadSafe> AudioSink = MakeShared<FTcpBlocksAudioSink, ESPMode::ThreadSafe>();
	const TSharedRef<FTcpBlocksControlSink, ESPMode::ThreadSafe> ControlSink = MakeShared<FTcpBlocksControlSink, ESPMode::ThreadSafe>();

	TestTrue(TEXT("Sender initializes"), Sender->Initialize(SenderConfig).IsOk());
	TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(MakeTcpConfig(false, Port, ReceiverOptions)).IsOk());
	Receiver->SetConsumer(Consumer);
	Receiver->SetAudioSink(AudioSink, SenderConfig.Audio);
	Receiver->SetControlSink(ControlSink);
	if (!TestTrue(TEXT("Sender starts"), Sender->Start().IsOk()) || !TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk()))
	{
		Receiver->Stop();
		Sender->Stop();
		return false;
	}
	if (!TestTrue(TEXT("Connected"), WaitFor(10.0, [&Sender, &Receiver]()
		{
			return O3DSocketsTesting::TcpReceiverIsConnected(*Receiver) && O3DSocketsTesting::TcpSenderHasClient(*Sender);
		})))
	{
		Receiver->Stop();
		Sender->Stop();
		return false;
	}

	// The receiver is not polled, so its hand-off queue, both kernel buffers and then the sender's
	// 64 KiB frame budget fill, and frames are refused.
	TArray<uint8> Frame;
	Frame.SetNumUninitialized(60 * 1024);
	for (int32 Index = 0; Index < Frame.Num(); ++Index)
	{
		Frame[Index] = static_cast<uint8>(Index * 13);
	}
	int32 Accepted = 0;
	bool bRefused = false;
	const double Deadline = FPlatformTime::Seconds() + 30.0;
	while (!bRefused && FPlatformTime::Seconds() < Deadline)
	{
		const EO3DSendResult Result = Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Fill"), 0.0));
		if (Result == EO3DSendResult::Queued)
		{
			++Accepted;
		}
		else if (Result == EO3DSendResult::DroppedBackpressure)
		{
			bRefused = true;
		}
		else
		{
			FPlatformProcess::YieldThread();
		}
	}
	TestTrue(TEXT("The frame budget filled up (a frame was refused)"), bRefused);

	// Audio and control have budgets of their own (ADR 0007 item 7, ADR 0011).
	const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> SenderAudio = Sender->CreateAudioSink(SenderConfig.Audio);
	const float Samples[8] = { 0.1f, 0.2f, 0.3f, 0.4f, -0.1f, -0.2f, -0.3f, -0.4f };
	TestTrue(TEXT("Audio accepted while frames are refused"), SenderAudio.IsValid() && SenderAudio->SubmitPcm(TEXT("voice"), Samples, 8, 1, 48000, 1.0));
	const TArray<uint8> Envelope = MakeControlEnvelope();
	TestTrue(TEXT("Control accepted while frames are refused"), Sender->SendControl(Envelope.GetData(), Envelope.Num()) == EO3DSendResult::Queued);
	TestTrue(TEXT("The refused frame is counted"), Sender->GetStats().DroppedFrames >= 1);

	// Everything accepted arrives once the receiver polls: TCP is ReliableOrdered.
	TestTrue(TEXT("Every accepted frame, the audio and the control arrive"), PollFor(*Receiver, 60.0, [&Consumer, &AudioSink, &ControlSink, Accepted]()
	{
		return Consumer->Num() >= Accepted && AudioSink->Calls.load() >= 1 && ControlSink->Calls.load() >= 1;
	}));
	TestEqual(TEXT("Frames received"), Consumer->Num(), Accepted);
	TestEqual(TEXT("One audio frame"), AudioSink->Calls.load(), 1);
	TestEqual(TEXT("One control payload"), ControlSink->Calls.load(), 1);
	TestEqual(TEXT("One connection throughout"), O3DSocketsTesting::TcpReceiverGetConnectCount(*Receiver), 1);

	Receiver->Stop();
	Sender->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpReceiverBackoffTest, "Open3DBroadcast.Transport.Sockets.Tcp.ReceiverBacksOffWithoutSender", O3DB_TEST_FLAGS)
bool FO3DSocketsTcpReceiverBackoffTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsTcpBlocksTests;
	const int32 Port = FindFreeTcpPort();
	if (!TestTrue(TEXT("Loopback TCP port allocated"), Port > 0))
	{
		return false;
	}

	TMap<FString, FString> ReceiverOptions;
	ReceiverOptions.Add(TEXT("tcp.backoff"), TEXT("100"));
	ReceiverOptions.Add(TEXT("tcp.maxbackoff"), TEXT("400"));
	ReceiverOptions.Add(TEXT("tcp.connecttimeout"), TEXT("1"));
	const TSharedRef<IOpen3DReceiver> Receiver = O3DSocketsTesting::CreateTcpReceiver();
	const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
	TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(MakeTcpConfig(false, Port, ReceiverOptions)).IsOk());
	Receiver->SetConsumer(Consumer);
	const double StartTime = FPlatformTime::Seconds();
	if (!TestTrue(TEXT("Receiver starts with no sender"), Receiver->Start().IsOk()))
	{
		return false;
	}

	// Nothing calls Poll: the worker attempts, fails and backs off on its own.
	TestTrue(TEXT("The worker retries without Poll"), WaitFor(15.0, [&Receiver]() { return O3DSocketsTesting::TcpReceiverGetFailedConnectAttempts(*Receiver) >= 3; }));
	const double Elapsed = FPlatformTime::Seconds() - StartTime;
	const int32 Attempts = O3DSocketsTesting::TcpReceiverGetFailedConnectAttempts(*Receiver);
	// With 100 ms doubling to 400 ms (+-20% jitter), attempts are at least 80 ms apart; a retry
	// loop without backoff would make hundreds in the same time.
	TestTrue(*FString::Printf(TEXT("Retries are spaced by the backoff (%d attempts in %.2f s)"), Attempts, Elapsed), Attempts <= 2 + static_cast<int32>(Elapsed / 0.08));
	TestFalse(TEXT("Not connected"), O3DSocketsTesting::TcpReceiverIsConnected(*Receiver));
	TestTrue(TEXT("State stays Connecting while the sender is missing"), Receiver->GetConnectionState() == EO3DConnectionState::Connecting);

	// The sender appears: the worker connects at its next attempt, still without Poll.
	const TSharedRef<IOpen3DSender> Sender = O3DSocketsTesting::CreateTcpSender();
	TestTrue(TEXT("Sender initializes"), Sender->Initialize(MakeTcpConfig(true, Port, {})).IsOk());
	TestTrue(TEXT("Sender starts"), Sender->Start().IsOk());
	TestTrue(TEXT("The receiver connects without Poll"), WaitFor(10.0, [&Receiver, &Sender]()
	{
		return O3DSocketsTesting::TcpReceiverIsConnected(*Receiver) && O3DSocketsTesting::TcpSenderHasClient(*Sender);
	}));
	TestEqual(TEXT("One connection"), O3DSocketsTesting::TcpReceiverGetConnectCount(*Receiver), 1);
	TestTrue(TEXT("State Connected"), Receiver->GetConnectionState() == EO3DConnectionState::Connected);

	// A connection that carries data resets the backoff (TRB-4).
	const uint8 Bytes[16] = { 'S', 'E', 'Q', ':', 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
	TestTrue(TEXT("Frame queued"), Sender->SendSerialized(FO3DSendPayload::MakeCopy(Bytes, 16, TEXT("Backoff"), 0.0)) == EO3DSendResult::Queued);
	TestTrue(TEXT("Frame received"), PollFor(*Receiver, 10.0, [&Consumer]() { return Consumer->Num() >= 1; }));
	TestEqual(TEXT("Data reset the backoff"), O3DSocketsTesting::TcpReceiverGetFailedConnectAttempts(*Receiver), 0);

	Receiver->Stop();
	Sender->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpStopUnderLoadTest, "Open3DBroadcast.Transport.Sockets.Tcp.StopWhileSending", O3DB_TEST_FLAGS)
bool FO3DSocketsTcpStopUnderLoadTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsTcpBlocksTests;
	// 1,000 cycles, as the ADR 0007 Verification case and the WP-S5 TCP stress test run. Most
	// cycles have no client (each send is then refused at once and Stop races the calls); every
	// 20th has a raw client that never reads, on a fresh port, so frames sit in the queue and on
	// the worker when Stop runs. 50 connected cycles keep the test short on CI.
	constexpr int32 ConnectedEvery = 20;
	const int32 IdlePort = FindFreeTcpPort();
	if (!TestTrue(TEXT("Loopback TCP port allocated"), IdlePort > 0))
	{
		return false;
	}

	TArray<TUniquePtr<FTcpSendHammer>> Hammers;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Hammers.Add(MakeUnique<FTcpSendHammer>());
	}
	O3DLifetimeTest::FFakeAudioThread AudioThread;

	int32 StartFailures = 0;
	int32 ConnectedCycles = 0;
	int32 AcceptedAfterStop = 0;
	int32 SlowStops = 0;
	for (int32 Cycle = 0; Cycle < O3DLifetimeTest::StressCycles; ++Cycle)
	{
		const bool bConnected = (Cycle % ConnectedEvery) == 0;
		const int32 Port = bConnected ? FindFreeTcpPort() : IdlePort;
		TMap<FString, FString> Options;
		Options.Add(TEXT("tcp.maxqueue"), TEXT("262144"));
		const FO3DTransportConfig Config = MakeTcpConfig(true, Port, Options);

		TSharedPtr<IOpen3DSender> Sender = O3DSocketsTesting::CreateTcpSender();
		if (Port <= 0 || !Sender->Initialize(Config).IsOk() || !Sender->Start().IsOk())
		{
			++StartFailures;
			continue;
		}

		FSocket* Client = nullptr;
		if (bConnected)
		{
			Client = ConnectRawClient(Port);
			if (Client && WaitFor(5.0, [&Sender]() { return O3DSocketsTesting::TcpSenderHasClient(*Sender); }))
			{
				++ConnectedCycles;
			}
		}

		AudioThread.SetSink(Sender->CreateAudioSink(Config.Audio));
		for (const TUniquePtr<FTcpSendHammer>& Hammer : Hammers)
		{
			Hammer->SetSender(Sender);
		}
		if (bConnected)
		{
			WaitFor(1.0, [&Sender]() { return Sender->GetStats().FramesSent > 0; });
		}
		FPlatformProcess::YieldThread();

		const double StopStart = FPlatformTime::Seconds();
		Sender->Stop();
		SlowStops += (FPlatformTime::Seconds() - StopStart) > 2.0 ? 1 : 0;
		const uint8 Probe[4] = { 9, 9, 9, 9 };
		AcceptedAfterStop += Sender->SendSerialized(FO3DSendPayload::MakeCopy(Probe, 4, TEXT("Probe"), 0.0)) == EO3DSendResult::Queued ? 1 : 0;
		DestroyRawClient(Client);

		// Half the cycles drop the sender while the threads still hold it.
		if ((Cycle & 1) == 0)
		{
			for (const TUniquePtr<FTcpSendHammer>& Hammer : Hammers)
			{
				Hammer->SetSender(nullptr);
			}
			AudioThread.SetSink(nullptr);
		}
		Sender.Reset();
	}

	int64 Calls = 0;
	for (TUniquePtr<FTcpSendHammer>& Hammer : Hammers)
	{
		Hammer->SetSender(nullptr);
		Hammer->StopAndJoin();
		Calls += Hammer->Calls.load();
	}
	AudioThread.SetSink(nullptr);
	AudioThread.StopAndJoin();

	TestEqual(TEXT("Every cycle started"), StartFailures, 0);
	TestEqual(TEXT("Every connected cycle had a client"), ConnectedCycles, O3DLifetimeTest::StressCycles / ConnectedEvery);
	TestEqual(TEXT("No send is accepted after Stop returned"), AcceptedAfterStop, 0);
	TestEqual(TEXT("Stop returned promptly every time"), SlowStops, 0);
	AddInfo(FString::Printf(TEXT("Send threads made %lld calls; the audio thread submitted %lld buffers"), Calls, AudioThread.GetSubmitted()));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
