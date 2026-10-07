// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U6 (TRB-22): the TCP listen socket and the UDP receive socket set SO_REUSEADDR, which on
// Windows lets a second socket bind a port that is already in use, so two senders or receivers
// shared a port silently (and another process could take it over). A second listener on a port in
// use must fail to start with AddressInUse. Loopback sockets only.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Misc/AutomationTest.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DSocketsPortSharingTests
{
	FO3DTransportConfig MakeConfig(FName Transport, EO3DTransportRole Role, const TCHAR* HostKey, int32 Port)
	{
		FO3DTransportConfig Config(Transport, Role);
		Config.Uri = FString::Printf(TEXT("%s://127.0.0.1:%d"), Transport == TEXT("TCP") ? TEXT("tcp") : TEXT("udp"), Port);
		Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
		Config.AdvancedParams.Add(HostKey, TEXT("127.0.0.1"));
		Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpPortInUseTest, "Open3DBroadcast.Transport.Sockets.Udp.SecondReceiverOnAPortFails", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpPortInUseTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsPortSharingTests;
	AddExpectedError(TEXT("Failed to bind UDP socket"), EAutomationExpectedMessageFlags::Contains, 1);

	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/false);
	if (!TestTrue(TEXT("UDP port allocated"), Port > 0))
	{
		return false;
	}
	const TSharedRef<FO3DRecordingFrameConsumer> Consumer = MakeShared<FO3DRecordingFrameConsumer>();
	const TSharedPtr<IOpen3DReceiver> First = FO3DTransportRegistry::Get().CreateReceiver(TEXT("UDP"));
	const TSharedPtr<IOpen3DReceiver> Second = FO3DTransportRegistry::Get().CreateReceiver(TEXT("UDP"));
	if (!TestTrue(TEXT("Receivers created"), First.IsValid() && Second.IsValid())
		|| !TestTrue(TEXT("First initializes"), First->Initialize(MakeConfig(TEXT("UDP"), EO3DTransportRole::Receiver, TEXT("host"), Port)).IsOk())
		|| !TestTrue(TEXT("Second initializes"), Second->Initialize(MakeConfig(TEXT("UDP"), EO3DTransportRole::Receiver, TEXT("host"), Port)).IsOk()))
	{
		return false;
	}
	First->SetConsumer(Consumer);
	Second->SetConsumer(Consumer);

	TestTrue(TEXT("The first receiver binds the port"), First->Start().IsOk());
	const FO3DTransportResult SecondStart = Second->Start();
	TestFalse(TEXT("A second receiver on the same port does not start"), SecondStart.IsOk());
	TestTrue(*FString::Printf(TEXT("It reports AddressInUse (got %s)"), LexToString(SecondStart.Code)), SecondStart.Code == EO3DTransportError::AddressInUse);

	Second->Stop();
	First->Stop();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsTcpPortInUseTest, "Open3DBroadcast.Transport.Sockets.Tcp.SecondSenderOnAPortFails", O3DB_TEST_FLAGS)
bool FO3DSocketsTcpPortInUseTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsPortSharingTests;
	AddExpectedError(TEXT("Bind failed on"), EAutomationExpectedMessageFlags::Contains, 1);

	const int32 Port = O3DTests::FindFreeLoopbackPort(/*bTcp=*/true);
	if (!TestTrue(TEXT("TCP port allocated"), Port > 0))
	{
		return false;
	}
	const TSharedPtr<IOpen3DSender> First = FO3DTransportRegistry::Get().CreateSender(TEXT("TCP"));
	const TSharedPtr<IOpen3DSender> Second = FO3DTransportRegistry::Get().CreateSender(TEXT("TCP"));
	if (!TestTrue(TEXT("Senders created"), First.IsValid() && Second.IsValid())
		|| !TestTrue(TEXT("First initializes"), First->Initialize(MakeConfig(TEXT("TCP"), EO3DTransportRole::Sender, TEXT("bind"), Port)).IsOk())
		|| !TestTrue(TEXT("Second initializes"), Second->Initialize(MakeConfig(TEXT("TCP"), EO3DTransportRole::Sender, TEXT("bind"), Port)).IsOk()))
	{
		return false;
	}

	TestTrue(TEXT("The first sender listens on the port"), First->Start().IsOk());
	const FO3DTransportResult SecondStart = Second->Start();
	TestFalse(TEXT("A second sender on the same port does not start"), SecondStart.IsOk());
	TestTrue(*FString::Printf(TEXT("It reports AddressInUse (got %s)"), LexToString(SecondStart.Code)), SecondStart.Code == EO3DTransportError::AddressInUse);

	Second->Stop();
	First->Stop();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
