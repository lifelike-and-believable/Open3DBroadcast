// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U6 (TRB-29; maintainer decision 2026-10-06): the listening ends (the TCP sender, the UDP
// receiver) default to 127.0.0.1, and a listener bound where other machines can reach it warns once
// per Start: the streams carry no authentication or encryption. Loopback and wildcard binds only.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Misc/AutomationTest.h"

#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsListenDefaultTest, "Open3DBroadcast.Transport.Sockets.ListenAddressDefaultsToLocalhost", O3DB_TEST_FLAGS)
bool FO3DSocketsListenDefaultTest::RunTest(const FString& Parameters)
{
	const TMap<FString, FString> NoOptions;
	struct FCase { const TCHAR* Transport; bool bSender; const TCHAR* Key; };
	const FCase Cases[] = {
		{ TEXT("TCP"), true, TEXT("bind") },
		{ TEXT("UDP"), false, TEXT("host") },
	};
	for (const FCase& Case : Cases)
	{
		const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(Case.Transport);
		if (!TestTrue(*FString::Printf(TEXT("%s is registered"), Case.Transport), Descriptor.IsValid()))
		{
			continue;
		}
		const FO3DTransportOptionSchema& Schema = Case.bSender ? Descriptor->SenderOptions.OptionSchema : Descriptor->ReceiverOptions.OptionSchema;
		FO3DTransportConfig Config(Case.Transport, Case.bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver);
		if (Case.bSender)
		{
			Descriptor->ConfigureSender(FO3DTransportOptionsView(NoOptions, &Schema), Config);
		}
		else
		{
			Descriptor->ConfigureReceiver(FO3DTransportOptionsView(NoOptions, &Schema), Config);
		}
		const FString* Written = Config.AdvancedParams.Find(Case.Key);
		TestTrue(*FString::Printf(TEXT("The %s %s listens on 127.0.0.1 by default (got %s)"), Case.Transport, Case.bSender ? TEXT("sender") : TEXT("receiver"), Written ? **Written : TEXT("nothing")),
			Written != nullptr && *Written == TEXT("127.0.0.1"));
	}

	TestTrue(TEXT("127.0.0.1 is loopback"), O3DTransportOptions::IsLoopbackHost(TEXT("127.0.0.1")));
	TestTrue(TEXT("127.5.6.7 is loopback"), O3DTransportOptions::IsLoopbackHost(TEXT("127.5.6.7")));
	TestTrue(TEXT("localhost is loopback"), O3DTransportOptions::IsLoopbackHost(TEXT("localhost")));
	TestTrue(TEXT("::1 is loopback"), O3DTransportOptions::IsLoopbackHost(TEXT("::1")));
	TestFalse(TEXT("0.0.0.0 is not loopback"), O3DTransportOptions::IsLoopbackHost(TEXT("0.0.0.0")));
	TestFalse(TEXT("192.168.1.10 is not loopback"), O3DTransportOptions::IsLoopbackHost(TEXT("192.168.1.10")));
	TestFalse(TEXT("An empty host is not loopback"), O3DTransportOptions::IsLoopbackHost(FString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsWildcardListenWarnsTest, "Open3DBroadcast.Transport.Sockets.ListeningBeyondLoopbackWarns", O3DB_TEST_FLAGS)
bool FO3DSocketsWildcardListenWarnsTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("reachable from other machines"), EAutomationExpectedMessageFlags::Contains, 2);

	const int32 TcpPort = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
	const int32 UdpPort = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
	if (!TestTrue(TEXT("Ports allocated"), TcpPort > 0 && UdpPort > 0))
	{
		return false;
	}

	// Built by the configure functions, as the sender component and the receiver source do.
	const FO3DTransportDescriptorPtr Tcp = FO3DTransportRegistry::Get().Find(TEXT("TCP"));
	const FO3DTransportDescriptorPtr Udp = FO3DTransportRegistry::Get().Find(TEXT("UDP"));
	if (!TestTrue(TEXT("TCP and UDP registered"), Tcp.IsValid() && Udp.IsValid()))
	{
		return false;
	}
	const TMap<FString, FString> SenderOptions = { { TEXT("bind"), TEXT("0.0.0.0") }, { TEXT("port"), FString::FromInt(TcpPort) } };
	FO3DTransportConfig SenderConfig(TEXT("TCP"), EO3DTransportRole::Sender);
	SenderConfig.AdvancedParams = SenderOptions;
	Tcp->ConfigureSender(FO3DTransportOptionsView(SenderOptions, &Tcp->SenderOptions.OptionSchema), SenderConfig);
	const TSharedPtr<IOpen3DSender> Sender = FO3DTransportRegistry::Get().CreateSender(TEXT("TCP"));
	TestTrue(TEXT("TCP sender starts on 0.0.0.0"), Sender.IsValid() && Sender->Initialize(SenderConfig).IsOk() && Sender->Start().IsOk());

	const TMap<FString, FString> ReceiverOptions = { { TEXT("host"), TEXT("0.0.0.0") }, { TEXT("port"), FString::FromInt(UdpPort) } };
	FO3DTransportConfig ReceiverConfig(TEXT("UDP"), EO3DTransportRole::Receiver);
	ReceiverConfig.AdvancedParams = ReceiverOptions;
	Udp->ConfigureReceiver(FO3DTransportOptionsView(ReceiverOptions, &Udp->ReceiverOptions.OptionSchema), ReceiverConfig);
	const TSharedPtr<IOpen3DReceiver> Receiver = FO3DTransportRegistry::Get().CreateReceiver(TEXT("UDP"));
	// Initialize releases the consumer, so it is set after Initialize and before Start.
	const bool bReceiverInitialized = Receiver.IsValid() && Receiver->Initialize(ReceiverConfig).IsOk();
	if (bReceiverInitialized)
	{
		Receiver->SetConsumer(MakeShared<FO3DRecordingFrameConsumer>());
	}
	TestTrue(TEXT("UDP receiver starts on 0.0.0.0"), bReceiverInitialized && Receiver->Start().IsOk());

	if (Receiver.IsValid())
	{
		Receiver->Stop();
	}
	if (Sender.IsValid())
	{
		Sender->Stop();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
