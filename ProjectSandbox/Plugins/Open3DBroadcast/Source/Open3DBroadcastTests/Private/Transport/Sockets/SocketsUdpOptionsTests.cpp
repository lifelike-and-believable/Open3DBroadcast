// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U3 (TRB-21, the UI part): the UDP receiver no longer offers an MTU option. Only the sender
// splits frames into fragments, so the MTU is a sender setting; the receiver keeps Max Datagram
// Bytes, the largest datagram it accepts. The registered "udp" schemas, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Misc/AutomationTest.h"

#include "Transport/O3DTransportRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsUdpOptionsTest, "Open3DBroadcast.Transport.Sockets.UdpMtuIsASenderOption", O3DB_TEST_FLAGS)
bool FO3DSocketsUdpOptionsTest::RunTest(const FString& Parameters)
{
	const auto HasKey = [](const FO3DTransportOptionSchema& Schema, const TCHAR* Key)
	{
		return Schema.ContainsByPredicate([Key](const FO3DTransportOptionField& Field) { return Field.Key == Key; });
	};

	FO3DTransportOptionSchema Sender;
	FO3DTransportOptionSchema Receiver;
	if (!TestTrue(TEXT("udp sender schema"), FO3DTransportRegistry::Get().GetOptionSchema(TEXT("udp"), EO3DTransportRole::Sender, Sender))
		|| !TestTrue(TEXT("udp receiver schema"), FO3DTransportRegistry::Get().GetOptionSchema(TEXT("udp"), EO3DTransportRole::Receiver, Receiver)))
	{
		return false;
	}

	TestTrue(TEXT("The sender has an MTU"), HasKey(Sender, TEXT("udp.mtu")));
	TestFalse(TEXT("The receiver has no MTU"), HasKey(Receiver, TEXT("udp.mtu")));
	TestTrue(TEXT("The receiver keeps Max Datagram Bytes"), HasKey(Receiver, TEXT("udp.maxdatagram")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
