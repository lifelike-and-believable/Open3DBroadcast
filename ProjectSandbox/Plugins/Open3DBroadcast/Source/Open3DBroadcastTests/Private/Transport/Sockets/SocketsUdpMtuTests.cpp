// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U6 (TRB-16): the UDP sender split a frame only above Max Datagram Bytes (64000), so the MTU
// did nothing for most frames and a large frame left as one datagram that IP had to fragment. A
// frame above the MTU is now sent as fragments no larger than the MTU, header included, and the
// sender's panel shows the MTU only (Max Datagram Bytes stays a hidden ceiling). No wire change:
// receivers already reassemble v2 fragments of any size up to their own Max Datagram Bytes.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Testing/SocketsTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DSocketsUdpMtuTests
{
	FSocket* BindRawReceiver(int32& OutPort)
	{
		OutPort = 0;
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bValid);
		Addr->SetPort(0);
		FSocket* Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UdpMtuRaw"), false);
		if (Socket && (!bValid || !Socket->Bind(*Addr)))
		{
			SocketSubsystem->DestroySocket(Socket);
			return nullptr;
		}
		if (Socket)
		{
			int32 Applied = 0;
			Socket->SetReceiveBufferSize(1 << 20, Applied);
			Socket->GetAddress(*Addr);
			OutPort = Addr->GetPort();
		}
		return Socket;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpFragmentAtMtuTest, "Open3DBroadcast.Transport.Sockets.Udp.FramesAboveTheMtuAreFragmented", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpFragmentAtMtuTest::RunTest(const FString& Parameters)
{
	using namespace O3DSocketsUdpMtuTests;
	constexpr int32 Mtu = 1200;
	constexpr int32 FragmentHeaderBytes = 24;
	constexpr int32 FrameBytes = 10240;
	constexpr int32 ExpectedFragments = (FrameBytes + (Mtu - FragmentHeaderBytes) - 1) / (Mtu - FragmentHeaderBytes);

	int32 Port = 0;
	FSocket* Raw = BindRawReceiver(Port);
	if (!TestNotNull(TEXT("Raw UDP socket bound"), Raw))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Raw);
	};

	FO3DTransportConfig Config(TEXT("UDP"), EO3DTransportRole::Sender);
	Config.Uri = FString::Printf(TEXT("udp://127.0.0.1:%d"), Port);
	Config.StreamId = FString::Printf(TEXT("127.0.0.1:%d"), Port);
	Config.AdvancedParams.Add(TEXT("host"), TEXT("127.0.0.1"));
	Config.AdvancedParams.Add(TEXT("port"), FString::FromInt(Port));
	Config.AdvancedParams.Add(TEXT("udp.mtu"), FString::FromInt(Mtu));
	const TSharedRef<IOpen3DSender> Sender = O3DSocketsTesting::CreateUdpSender();
	if (!TestTrue(TEXT("Sender initializes"), Sender->Initialize(Config).IsOk()) || !TestTrue(TEXT("Sender starts"), Sender->Start().IsOk()))
	{
		return false;
	}

	TArray<uint8> Frame;
	Frame.SetNumUninitialized(FrameBytes);
	for (int32 Index = 0; Index < FrameBytes; ++Index)
	{
		Frame[Index] = static_cast<uint8>(Index * 7 + 3);
	}
	TestTrue(TEXT("Frame queued"), Sender->SendSerialized(FO3DSendPayload::MakeCopy(Frame.GetData(), Frame.Num(), TEXT("Mtu"), 0.0)) == EO3DSendResult::Queued);

	TArray<int32> Sizes;
	int32 NotFragments = 0;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(65536);
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (Sizes.Num() < ExpectedFragments && FPlatformTime::Seconds() < Deadline)
	{
		if (!Raw->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(50)))
		{
			continue;
		}
		int32 Read = 0;
		TSharedRef<FInternetAddr> From = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		if (Raw->RecvFrom(Buffer.GetData(), Buffer.Num(), Read, *From) && Read > 0)
		{
			Sizes.Add(Read);
			NotFragments += (Read < 4 || FMemory::Memcmp(Buffer.GetData(), "O3DF", 4) != 0) ? 1 : 0;
		}
	}
	Sender->Stop();

	TestEqual(TEXT("The frame arrives as MTU-sized fragments"), Sizes.Num(), ExpectedFragments);
	TestEqual(TEXT("Every datagram is a fragment"), NotFragments, 0);
	for (int32 Size : Sizes)
	{
		if (!TestTrue(*FString::Printf(TEXT("A %d-byte datagram fits the %d-byte MTU"), Size, Mtu), Size <= Mtu))
		{
			break;
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpSenderPanelTest, "Open3DBroadcast.Transport.Sockets.Udp.SenderPanelShowsTheMtuOnly", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpSenderPanelTest::RunTest(const FString& Parameters)
{
	FO3DTransportOptionSchema Sender;
	FO3DTransportOptionSchema Receiver;
	if (!TestTrue(TEXT("UDP sender schema"), FO3DTransportRegistry::Get().GetOptionSchema(TEXT("UDP"), EO3DTransportRole::Sender, Sender))
		|| !TestTrue(TEXT("UDP receiver schema"), FO3DTransportRegistry::Get().GetOptionSchema(TEXT("UDP"), EO3DTransportRole::Receiver, Receiver)))
	{
		return false;
	}
	const auto Find = [](const FO3DTransportOptionSchema& Schema, const TCHAR* Key)
	{
		return Schema.FindByPredicate([Key](const FO3DTransportOptionField& Field) { return Field.Key == Key; });
	};

	const FO3DTransportOptionField* Mtu = Find(Sender, TEXT("udp.mtu"));
	if (TestNotNull(TEXT("The sender shows the MTU"), Mtu))
	{
		TestEqual(TEXT("The MTU holds at least a 24-byte fragment header and 256 bytes"), Mtu->Min, 280.0);
	}
	TestNull(TEXT("The sender no longer shows Max Datagram Bytes"), Find(Sender, TEXT("udp.maxdatagram")));
	TestNotNull(TEXT("The receiver keeps Max Datagram Bytes"), Find(Receiver, TEXT("udp.maxdatagram")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
