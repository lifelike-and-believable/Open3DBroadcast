// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U6 (TRB-21; maintainer decision 2026-10-06): UDP gains multicast (the receiver joins an IPv4
// group, the sender sets TTL and loopback), an optional allow-list of sender addresses, and an
// opt-in to share a port between receivers on one machine. The receiver's Accept Broadcast Packets
// option did nothing (SO_BROADCAST only matters for sending) and is gone. Loopback and the
// administratively scoped multicast range only.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DSocketsUdpMulticastTests
{
	/** A config built by the registered configure function, as the sender component and receiver source do. */
	FO3DTransportConfig Configure(EO3DTransportRole Role, const TMap<FString, FString>& Options)
	{
		const FO3DTransportDescriptorPtr Udp = FO3DTransportRegistry::Get().Find(TEXT("UDP"));
		FO3DTransportConfig Config(TEXT("UDP"), Role);
		Config.AdvancedParams = Options;
		if (Udp.IsValid())
		{
			if (Role == EO3DTransportRole::Sender)
			{
				Udp->ConfigureSender(FO3DTransportOptionsView(Options, &Udp->SenderOptions.OptionSchema), Config);
			}
			else
			{
				Udp->ConfigureReceiver(FO3DTransportOptionsView(Options, &Udp->ReceiverOptions.OptionSchema), Config);
			}
		}
		return Config;
	}

	TSharedPtr<IOpen3DReceiver> StartReceiver(FAutomationTestBase& Test, const TMap<FString, FString>& Options, const TSharedRef<FO3DRecordingFrameConsumer>& Consumer)
	{
		TSharedPtr<IOpen3DReceiver> Receiver = FO3DTransportRegistry::Get().CreateReceiver(TEXT("UDP"));
		if (!Test.TestTrue(TEXT("Receiver created"), Receiver.IsValid())
			|| !Test.TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(Configure(EO3DTransportRole::Receiver, Options)).IsOk()))
		{
			return nullptr;
		}
		Receiver->SetConsumer(Consumer); // after Initialize, which releases the consumer
		if (!Test.TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk()))
		{
			return nullptr;
		}
		return Receiver;
	}

	void PollUntilFrames(const TArray<TSharedPtr<IOpen3DReceiver>>& Receivers, const TArray<TSharedRef<FO3DRecordingFrameConsumer>>& Consumers, double Seconds)
	{
		O3DTests::PollUntil(Seconds,
			[&Consumers]()
			{
				for (const TSharedRef<FO3DRecordingFrameConsumer>& Consumer : Consumers)
				{
					if (Consumer->Num() == 0)
					{
						return false;
					}
				}
				return true;
			},
			[&Receivers]()
			{
				for (const TSharedPtr<IOpen3DReceiver>& Receiver : Receivers)
				{
					Receiver->Poll();
				}
			});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpMulticastOptionsTest, "Open3DBroadcast.Transport.Sockets.Udp.MulticastOptions", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpMulticastOptionsTest::RunTest(const FString& Parameters)
{
	FO3DTransportOptionSchema Sender;
	FO3DTransportOptionSchema Receiver;
	if (!TestTrue(TEXT("UDP sender schema"), FO3DTransportRegistry::Get().GetOptionSchema(TEXT("UDP"), EO3DTransportRole::Sender, Sender))
		|| !TestTrue(TEXT("UDP receiver schema"), FO3DTransportRegistry::Get().GetOptionSchema(TEXT("UDP"), EO3DTransportRole::Receiver, Receiver)))
	{
		return false;
	}
	const auto Has = [](const FO3DTransportOptionSchema& Schema, const TCHAR* Key)
	{
		return Schema.ContainsByPredicate([Key](const FO3DTransportOptionField& Field) { return Field.Key == Key; });
	};
	TestTrue(TEXT("The receiver can join a multicast group"), Has(Receiver, TEXT("udp.multicast")));
	TestTrue(TEXT("The receiver can limit its senders"), Has(Receiver, TEXT("udp.allowsource")));
	TestTrue(TEXT("The receiver can share its port"), Has(Receiver, TEXT("udp.reuseaddr")));
	TestFalse(TEXT("The receiver has no broadcast option"), Has(Receiver, TEXT("udp.broadcast")));
	TestTrue(TEXT("The sender keeps its broadcast option"), Has(Sender, TEXT("udp.broadcast")));
	TestTrue(TEXT("The sender sets the multicast TTL"), Has(Sender, TEXT("udp.multicastttl")));
	TestTrue(TEXT("The sender sets multicast loopback"), Has(Sender, TEXT("udp.multicastloop")));

	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
	const TSharedPtr<IOpen3DReceiver> Probe = FO3DTransportRegistry::Get().CreateReceiver(TEXT("UDP"));
	if (TestTrue(TEXT("Receiver created"), Probe.IsValid()))
	{
		const FO3DTransportResult NotAGroup = Probe->Initialize(O3DSocketsUdpMulticastTests::Configure(EO3DTransportRole::Receiver,
			{ { TEXT("port"), FString::FromInt(Port) }, { TEXT("udp.multicast"), TEXT("192.168.1.1") } }));
		TestTrue(TEXT("A unicast address is not a multicast group (InvalidConfig)"), NotAGroup.Code == EO3DTransportError::InvalidConfig);
		const FO3DTransportResult BadSource = Probe->Initialize(O3DSocketsUdpMulticastTests::Configure(EO3DTransportRole::Receiver,
			{ { TEXT("port"), FString::FromInt(Port) }, { TEXT("udp.allowsource"), TEXT("127.0.0.1, not-an-address") } }));
		TestTrue(TEXT("An allowed sender must be an IP address (InvalidConfig)"), BadSource.Code == EO3DTransportError::InvalidConfig);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpAllowSourceTest, "Open3DBroadcast.Transport.Sockets.Udp.AllowedSendersOnly", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpAllowSourceTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsUdpMulticastTests;
	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("AllowSource"), 1);
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Raw = SocketSubsystem ? SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UdpAllowSource"), false) : nullptr;
	if (!TestEqual(TEXT("Frame built"), Frames.Num(), 1) || !TestNotNull(TEXT("Raw socket"), Raw))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		SocketSubsystem->DestroySocket(Raw);
	};

	// The raw socket sends from 127.0.0.1. A receiver that allows only 127.0.0.2 drops it; one that
	// allows 127.0.0.1 delivers it.
	for (const bool bAllowed : { false, true })
	{
		const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
		const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
		const TSharedPtr<IOpen3DReceiver> Receiver = StartReceiver(*this,
			{ { TEXT("host"), TEXT("127.0.0.1") }, { TEXT("port"), FString::FromInt(Port) }, { TEXT("udp.allowsource"), bAllowed ? TEXT("127.0.0.1") : TEXT("127.0.0.2") } },
			Consumer);
		if (!Receiver.IsValid())
		{
			return false;
		}

		TSharedRef<FInternetAddr> To = SocketSubsystem->CreateInternetAddr();
		bool bValid = false;
		To->SetIp(TEXT("127.0.0.1"), bValid);
		To->SetPort(Port);
		int32 Sent = 0;
		Raw->SendTo(Frames[0].GetData(), Frames[0].Num(), Sent, *To);

		O3DTests::PollUntil(bAllowed ? 2.0 : 0.5, [&Consumer]() { return Consumer->Num() > 0; }, [&Receiver]() { Receiver->Poll(); });
		if (bAllowed)
		{
			TestEqual(TEXT("A frame from an allowed sender is delivered"), Consumer->Num(), 1);
		}
		else
		{
			TestEqual(TEXT("A frame from any other sender is dropped"), Consumer->Num(), 0);
			TestTrue(TEXT("The drop is counted as a receive error"), Receiver->GetStats().ReceiveErrors >= 1);
		}
		Receiver->Stop();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpMulticastRoundTripTest, "Open3DBroadcast.Transport.Sockets.Udp.MulticastReachesEveryJoinedReceiver", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpMulticastRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsUdpMulticastTests;
	// Both receivers bind 0.0.0.0, which other machines can reach, so each warns once.
	AddExpectedError(TEXT("reachable from other machines"), EAutomationExpectedMessageFlags::Contains, 2);

	const TCHAR* Group = TEXT("239.255.79.51"); // administratively scoped (RFC 2365): never leaves the site
	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
	const TArray<TArray<uint8>> Frames = O3DTests::MakeRecordedFrames(TEXT("Multicast"), 1);
	if (!TestTrue(TEXT("Port allocated"), Port > 0) || !TestEqual(TEXT("Frame built"), Frames.Num(), 1))
	{
		return false;
	}

	const TMap<FString, FString> ReceiverOptions = {
		{ TEXT("host"), TEXT("0.0.0.0") },
		{ TEXT("port"), FString::FromInt(Port) },
		{ TEXT("udp.multicast"), Group },
		{ TEXT("udp.reuseaddr"), TEXT("true") },
	};
	const TArray<TSharedRef<FO3DRecordingFrameConsumer>> Consumers = { MakeShared<FO3DRecordingFrameConsumer>(), MakeShared<FO3DRecordingFrameConsumer>() };
	TArray<TSharedPtr<IOpen3DReceiver>> Receivers;
	for (const TSharedRef<FO3DRecordingFrameConsumer>& Consumer : Consumers)
	{
		const TSharedPtr<IOpen3DReceiver> Receiver = StartReceiver(*this, ReceiverOptions, Consumer);
		if (!Receiver.IsValid())
		{
			for (const TSharedPtr<IOpen3DReceiver>& Started : Receivers)
			{
				Started->Stop();
			}
			return false;
		}
		Receivers.Add(Receiver);
	}

	const TSharedPtr<IOpen3DSender> Sender = FO3DTransportRegistry::Get().CreateSender(TEXT("UDP"));
	const FO3DTransportConfig SenderConfig = Configure(EO3DTransportRole::Sender,
		{ { TEXT("host"), Group }, { TEXT("port"), FString::FromInt(Port) }, { TEXT("udp.multicastloop"), TEXT("true") } });
	if (TestTrue(TEXT("Sender starts"), Sender.IsValid() && Sender->Initialize(SenderConfig).IsOk() && Sender->Start().IsOk()))
	{
		// UDP may drop a datagram; resend until both receivers have one or the time is up.
		const double Deadline = FPlatformTime::Seconds() + 3.0;
		while (FPlatformTime::Seconds() < Deadline && (Consumers[0]->Num() == 0 || Consumers[1]->Num() == 0))
		{
			Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frames[0].GetData(), Frames[0].Num(), TEXT("Multicast"), 0.0));
			PollUntilFrames(Receivers, Consumers, 0.2);
		}
		TestTrue(TEXT("The first receiver joined the group and got the frame"), Consumers[0]->Num() > 0);
		TestTrue(TEXT("The second receiver on the same port got it too"), Consumers[1]->Num() > 0);
		Sender->Stop();
	}
	for (const TSharedPtr<IOpen3DReceiver>& Receiver : Receivers)
	{
		Receiver->Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
