// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-R3 (mid-project review TR-6): datagrams any peer can send must not produce a log line each.
// A UDP receiver on 127.0.0.1 that only takes 512-byte datagrams is sent oversized ones from a
// plain socket; it warns once, not once per datagram.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Misc/AutomationTest.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Transport/O3DTransportRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpOversizeThrottleTest, "Open3DBroadcast.Transport.Sockets.Udp.OversizedDatagramWarningIsThrottled", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpOversizeThrottleTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("UDP datagram too large"), EAutomationExpectedMessageFlags::Contains, 1);

	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
	if (!TestTrue(TEXT("Loopback UDP port allocated"), Port > 0))
	{
		return false;
	}
	FO3DTransportConfig Config;
	Config.Transport = TEXT("UDP");
	Config.Role = EO3DTransportRole::Receiver;
	Config.Uri = FString::Printf(TEXT("udp://127.0.0.1:%d"), Port);
	Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
	Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
	Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
	Config.AdvancedParams.Add(TEXT("udp.maxdatagram"), TEXT("512"));

	const TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver = FO3DTransportRegistry::Get().CreateReceiver(TEXT("UDP"));
	if (!TestTrue(TEXT("UDP receiver created"), Receiver.IsValid())
		|| !TestTrue(TEXT("Receiver initializes"), Receiver->Initialize(Config).IsOk()))
	{
		return false;
	}
	Receiver->SetConsumer(MakeShared<FO3DRecordingFrameConsumer>());
	if (!TestTrue(TEXT("Receiver starts"), Receiver->Start().IsOk()))
	{
		return false;
	}

	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* Socket = Subsystem ? Subsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UdpOversizeTest"), false) : nullptr;
	if (!TestNotNull(TEXT("Sending socket"), Socket))
	{
		Receiver->Stop();
		return false;
	}
	TSharedRef<FInternetAddr> Address = Subsystem->CreateInternetAddr();
	bool bValid = false;
	Address->SetIp(TEXT("127.0.0.1"), bValid);
	Address->SetPort(Port);

	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(2000);
	constexpr int32 Datagrams = 5;
	for (int32 Index = 0; Index < Datagrams; ++Index)
	{
		int32 Sent = 0;
		Socket->SendTo(Oversized.GetData(), Oversized.Num(), Sent, *Address);
	}
	// Poll for a second, so all five are read; each is refused as too large (the expected-error
	// count above is the check).
	O3DTests::PollUntil(1.0, []() { return false; }, [&Receiver]() { Receiver->Poll(); });
	TestEqual(TEXT("No oversized datagram reached the consumer"), Receiver->GetStats().FramesReceived, static_cast<int64>(0));

	Subsystem->DestroySocket(Socket);
	Receiver->Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
