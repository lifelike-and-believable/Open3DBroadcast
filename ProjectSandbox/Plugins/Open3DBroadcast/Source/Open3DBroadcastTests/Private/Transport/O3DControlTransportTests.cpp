// Copyright Lifelike & Believable. All Rights Reserved.

//
// ADR 0011 (CTL-3): transport-specific control behaviour the conformance suite does not cover.
//   - NNG carries control in all three socket pairings: pub/sub, pair/pair, push/pull.
//   - UDP delivers control as single datagrams (including the largest envelope, which must not be
//     mistaken for a fragment) and refuses an envelope larger than udp.maxdatagram rather than
//     fragmenting it.
//   - Loopback keeps control in its own queue: a full frame queue does not block control, and the
//     control queue has its own cap.
// Everything runs on 127.0.0.1 or in process; nothing needs an external network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DSenderInterface.h"
#include "Transport/O3DTransportRegistry.h"

#if O3D_WITH_TRANSPORT_NNG
#include "Testing/NngTesting.h"
#endif
#if O3D_WITH_TRANSPORT_SOCKETS
#include "Testing/SocketsTesting.h"
#endif

namespace O3DControlTransportTests
{
	constexpr double TimeoutSeconds = 10.0;
	constexpr double ProbeIntervalSeconds = 0.05;
	constexpr int32 ControlCount = 6;

	class FControlRecordingSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& /*StreamId*/, double /*ReceiveTimeSec*/) override
		{
			FScopeLock Lock(&Mutex);
			Payloads.Emplace(Payload.GetData(), Payload.Num());
		}
		TArray<TArray<uint8>> Get() const
		{
			FScopeLock Lock(&Mutex);
			return Payloads;
		}
		bool Contains(const TArray<uint8>& Payload) const
		{
			FScopeLock Lock(&Mutex);
			return Payloads.Contains(Payload);
		}

	private:
		mutable FCriticalSection Mutex;
		TArray<TArray<uint8>> Payloads;
	};

	/** Distinct bytes per index; every third one is the largest payload an envelope allows. */
	TArray<uint8> MakePayload(int32 Index)
	{
		const int32 Size = (Index % 3 == 2) ? O3DS::UnifiedMaxControlPayloadSize : 20 + Index * 53;
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(Size);
		for (int32 Byte = 0; Byte < Size; ++Byte)
		{
			Payload[Byte] = static_cast<uint8>((Index * 197 + Byte * 13 + 5) & 0xFF);
		}
		return Payload;
	}

	TArray<uint8> MakeEnvelope(int32 Index)
	{
		TArray<uint8> Envelope;
		O3DS::WriteControlEnvelope(MakePayload(Index), 1.0 + Index, Envelope);
		return Envelope;
	}

	/**
	 * Starts both sides (bSenderFirst picks which side starts first, so the listening side is up
	 * before the dialing one), exchanges a probe frame, then sends ControlCount control envelopes.
	 * Reliable transports must deliver each once and in order; unreliable ones are re-sent until
	 * every payload has arrived at least once. No control may reach the frame consumer.
	 */
	bool RunControlPair(FAutomationTestBase& Test, const TSharedRef<IOpen3DSender>& Sender, const TSharedRef<IOpen3DReceiver>& Receiver,
		const FO3DTransportConfig& SenderConfig, const FO3DTransportConfig& ReceiverConfig, bool bSenderFirst, bool bReliable)
	{
		const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		const TSharedRef<FControlRecordingSink, ESPMode::ThreadSafe> Sink = MakeShared<FControlRecordingSink, ESPMode::ThreadSafe>();
		ON_SCOPE_EXIT
		{
			Receiver->Stop();
			Sender->Stop();
		};

		if (!Test.TestTrue(TEXT("Sender supports control"), Sender->SupportsControl())
			|| !Test.TestTrue(TEXT("Receiver supports control"), Receiver->SupportsControl())
			|| !Test.TestTrue(TEXT("Sender initializes"), Sender->Initialize(SenderConfig))
			|| !Test.TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(ReceiverConfig)))
		{
			return false;
		}
		Receiver->SetConsumer(Consumer);
		Receiver->SetControlSink(Sink);
		const bool bStarted = bSenderFirst
			? (Test.TestTrue(TEXT("Sender starts"), Sender->Start()) && Test.TestTrue(TEXT("Receiver starts"), Receiver->Start()))
			: (Test.TestTrue(TEXT("Receiver starts"), Receiver->Start()) && Test.TestTrue(TEXT("Sender starts"), Sender->Start()));
		if (!bStarted)
		{
			return false;
		}
		auto Pump = [&Sender, &Receiver]()
		{
			Receiver->Poll();
			Sender->Tick(0.0f);
		};

		const TArray<TArray<uint8>> ProbeFrames = O3DTests::MakeRecordedFrames(TEXT("__o3d_control_probe__"), 1);
		const TArray<uint8>& Probe = ProbeFrames[0];
		double NextProbe = 0.0;
		const bool bConnected = O3DTests::PollUntil(TimeoutSeconds,
			[&Consumer]() { return Consumer->Num() > 0; },
			[&Pump, &Sender, &Probe, &NextProbe]()
			{
				Pump();
				const double Now = FPlatformTime::Seconds();
				if (Now >= NextProbe)
				{
					NextProbe = Now + ProbeIntervalSeconds;
					Sender->SendSerialized(Probe.GetData(), Probe.Num(), TEXT("probe"), Now);
				}
			});
		if (!Test.TestTrue(TEXT("Sender and receiver exchange a probe frame"), bConnected))
		{
			return false;
		}

		for (int32 Index = 0; Index < ControlCount; ++Index)
		{
			const TArray<uint8> Envelope = MakeEnvelope(Index);
			const TArray<uint8> Payload = MakePayload(Index);
			if (bReliable)
			{
				const bool bQueued = O3DTests::PollUntil(TimeoutSeconds,
					[&Sender, &Envelope]() { return Sender->SendControl(Envelope.GetData(), Envelope.Num()); }, Pump);
				Test.TestTrue(*FString::Printf(TEXT("Control %d accepted"), Index), bQueued);
			}
			else
			{
				// Best effort: re-send until this payload has arrived (localhost rarely loses any).
				double NextSend = 0.0;
				const bool bArrived = O3DTests::PollUntil(TimeoutSeconds,
					[&Sink, &Payload]() { return Sink->Contains(Payload); },
					[&Pump, &Sender, &Envelope, &NextSend]()
					{
						Pump();
						const double Now = FPlatformTime::Seconds();
						if (Now >= NextSend)
						{
							NextSend = Now + 0.2;
							Sender->SendControl(Envelope.GetData(), Envelope.Num());
						}
					});
				Test.TestTrue(*FString::Printf(TEXT("Control %d arrived"), Index), bArrived);
			}
		}

		if (bReliable)
		{
			O3DTests::PollUntil(TimeoutSeconds, [&Sink]() { return Sink->Get().Num() >= ControlCount; }, Pump);
			const TArray<TArray<uint8>> Received = Sink->Get();
			Test.TestEqual(TEXT("Every control payload arrived exactly once"), Received.Num(), ControlCount);
			bool bExact = Received.Num() == ControlCount;
			for (int32 Index = 0; bExact && Index < Received.Num(); ++Index)
			{
				bExact = Received[Index] == MakePayload(Index);
			}
			Test.TestTrue(TEXT("Control payloads byte-exact and in order"), bExact);
		}

		for (const TArray<uint8>& Frame : Consumer->GetFrames())
		{
			Test.TestTrue(TEXT("Only probe frames reached the frame consumer (no control leaked into it)"), Frame == Probe);
		}
		return true;
	}
}

#if O3D_WITH_TRANSPORT_NNG

namespace O3DControlTransportTests
{
	FO3DTransportConfig MakeNngConfig(bool bSender, const TCHAR* Mode, int32 Port)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("nng");
		Config.Role = bSender ? TEXT("sender") : TEXT("receiver");
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("nng.mode"), Mode);
		Config.AdvancedParams.Add(TEXT("nng.role"), bSender ? TEXT("server") : TEXT("client"));
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlNngModesTest, "Open3DBroadcast.Transport.NNG.ControlInEveryMode", O3DB_TEST_FLAGS)
bool FO3DControlNngModesTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlTransportTests;
	const TCHAR* const Modes[][2] = { { TEXT("pub"), TEXT("sub") }, { TEXT("pair"), TEXT("pair") }, { TEXT("push"), TEXT("pull") } };
	for (const auto& Mode : Modes)
	{
		AddInfo(FString::Printf(TEXT("NNG %s -> %s"), Mode[0], Mode[1]));
		const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
		if (!TestTrue(TEXT("Ephemeral loopback port allocated"), Port > 0))
		{
			return false;
		}
		RunControlPair(*this, O3DNngTesting::CreateSender(), O3DNngTesting::CreateReceiver(),
			MakeNngConfig(true, Mode[0], Port), MakeNngConfig(false, Mode[1], Port), /*bSenderFirst=*/true, /*bReliable=*/true);
	}
	return true;
}

#endif // O3D_WITH_TRANSPORT_NNG

#if O3D_WITH_TRANSPORT_SOCKETS

namespace O3DControlTransportTests
{
	FO3DTransportConfig MakeUdpConfig(bool bSender, int32 Port, int32 MaxDatagramBytes = 0)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("sockets.udp");
		Config.Role = bSender ? TEXT("sender") : TEXT("receiver");
		Config.Uri = FString::Printf(TEXT("udp://127.0.0.1:%d"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		if (MaxDatagramBytes > 0)
		{
			Config.AdvancedParams.Add(TEXT("udp.maxdatagram"), FString::FromInt(MaxDatagramBytes));
		}
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlUdpDeliveryTest, "Open3DBroadcast.Transport.Sockets.UdpControlDatagrams", O3DB_TEST_FLAGS)
bool FO3DControlUdpDeliveryTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlTransportTests;
	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
	if (!TestTrue(TEXT("Ephemeral loopback port allocated"), Port > 0))
	{
		return false;
	}
	// Includes two largest-size envelopes: they must arrive as control, not be read as UDP fragments.
	RunControlPair(*this, O3DSocketsTesting::CreateUdpSender(), O3DSocketsTesting::CreateUdpReceiver(),
		MakeUdpConfig(true, Port), MakeUdpConfig(false, Port), /*bSenderFirst=*/false, /*bReliable=*/false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlUdpNoFragmentTest, "Open3DBroadcast.Transport.Sockets.UdpControlIsNeverFragmented", O3DB_TEST_FLAGS)
bool FO3DControlUdpNoFragmentTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlTransportTests;
	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
	const TSharedRef<IOpen3DSender> Sender = O3DSocketsTesting::CreateUdpSender();
	ON_SCOPE_EXIT { Sender->Stop(); };
	// udp.maxdatagram below the largest control envelope (1,096 bytes).
	if (!TestTrue(TEXT("Initialize"), Sender->Initialize(MakeUdpConfig(true, Port, 512))) || !TestTrue(TEXT("Start"), Sender->Start()))
	{
		return false;
	}
	const TArray<uint8> Large = MakeEnvelope(2); // largest payload
	const TArray<uint8> Small = MakeEnvelope(0);
	TestTrue(TEXT("The large envelope exceeds the configured datagram size"), Large.Num() > 512);
	TestFalse(TEXT("An envelope larger than udp.maxdatagram is refused, not fragmented"), Sender->SendControl(Large.GetData(), Large.Num()));
	TestTrue(TEXT("A small envelope is sent"), Sender->SendControl(Small.GetData(), Small.Num()));
	TestEqual(TEXT("Control is not counted as a frame"), Sender->GetStats().FramesSent, static_cast<int64>(0));
	return true;
}

#endif // O3D_WITH_TRANSPORT_SOCKETS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DControlLoopbackQueuesTest, "Open3DBroadcast.Transport.Loopback.ControlQueueIsIndependent", O3DB_TEST_FLAGS)
bool FO3DControlLoopbackQueuesTest::RunTest(const FString& Parameters)
{
	using namespace O3DControlTransportTests;
	const FName Loopback(TEXT("Loopback"));
	const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = FO3DTransportRegistry::Get().CreateSender(Loopback);
	const TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver = FO3DTransportRegistry::Get().CreateReceiver(Loopback);
	if (!TestTrue(TEXT("Loopback is registered"), Sender.IsValid() && Receiver.IsValid()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Receiver->Stop();
		Sender->Stop();
	};

	const FString Channel = O3DTests::MakeUniqueName(TEXT("O3DControlLoopback"));
	auto MakeConfig = [&Channel](bool bSender)
	{
		FO3DTransportConfig Config;
		Config.Transport = TEXT("Loopback");
		Config.Role = bSender ? TEXT("sender") : TEXT("receiver");
		Config.StreamId = Channel;
		Config.Uri = FString::Printf(TEXT("loopback://%s?role=%s"), *Channel, bSender ? TEXT("pub") : TEXT("sub"));
		Config.AdvancedParams.Add(TEXT("channel"), Channel);
		Config.AdvancedParams.Add(TEXT("loopback.maxqueue"), TEXT("1"));
		return Config;
	};

	const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
	const TSharedRef<FControlRecordingSink, ESPMode::ThreadSafe> Sink = MakeShared<FControlRecordingSink, ESPMode::ThreadSafe>();
	TestTrue(TEXT("Sender initializes and starts"), Sender->Initialize(MakeConfig(true)) && Sender->Start());
	TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(MakeConfig(false)));
	Receiver->SetConsumer(Consumer);
	Receiver->SetControlSink(Sink);
	TestTrue(TEXT("Receiver starts"), Receiver->Start());

	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("LoopbackControlActor"), 2);
	TestTrue(TEXT("First frame fills the one-frame queue"), Sender->SendSerialized(Frames[0].GetData(), Frames[0].Num(), TEXT("A"), 0.0));
	TestFalse(TEXT("Second frame is refused: the frame queue is full"), Sender->SendSerialized(Frames[1].GetData(), Frames[1].Num(), TEXT("A"), 0.0));

	// Control is unaffected by the full frame queue, up to its own cap.
	int32 Accepted = 0;
	for (int32 Index = 0; Index < 2000; ++Index)
	{
		const TArray<uint8> Envelope = MakeEnvelope(Index % 2); // small envelopes
		if (!Sender->SendControl(Envelope.GetData(), Envelope.Num()))
		{
			break;
		}
		++Accepted;
	}
	TestEqual(TEXT("Control accepted up to its own cap while frames are blocked"), Accepted, 1024);

	TestEqual(TEXT("Poll counts only the frame"), Receiver->Poll(), 1);
	TestEqual(TEXT("Every control payload delivered"), Sink->Get().Num(), Accepted);
	TestEqual(TEXT("The consumer received only the frame"), Consumer->Num(), 1);

	const TArray<uint8> Envelope = MakeEnvelope(0);
	TestTrue(TEXT("Control is accepted again once drained"), Sender->SendControl(Envelope.GetData(), Envelope.Num()));
	Receiver->Stop();
	Receiver->Poll();
	TestEqual(TEXT("Stop released the control sink: nothing more delivered"), Sink->Get().Num(), Accepted);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
