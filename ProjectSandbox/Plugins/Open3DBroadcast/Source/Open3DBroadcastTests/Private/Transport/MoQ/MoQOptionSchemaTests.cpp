// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-R3 (mid-project review TR-8): the MoQ receiver's options panel showed Delivery Mode and
// Queue Capacity, which only the sender reads (the publisher picks the delivery; the receiver has
// no send queue). Each side's schema lists only the options that side uses.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_MOQ

#include "Misc/AutomationTest.h"
#include "Transport/O3DTransportRegistry.h"

namespace MoQOptionSchemaTest
{
	bool HasKey(const FO3DTransportOptionSchema& Schema, const TCHAR* Key)
	{
		return Schema.ContainsByPredicate([Key](const FO3DTransportOptionField& Field) { return Field.Key == Key; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMoQOptionSchemaSidesTest, "Open3DBroadcast.Transport.MoQ.Options.EachSideListsWhatItUses", O3DB_TEST_FLAGS)
bool FMoQOptionSchemaSidesTest::RunTest(const FString& Parameters)
{
	using namespace MoQOptionSchemaTest;

	const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(TEXT("MoQ"));
	if (!TestTrue(TEXT("MoQ is registered"), Descriptor.IsValid()))
	{
		return false;
	}
	const FO3DTransportOptionSchema& Sender = Descriptor->SenderOptions.OptionSchema;
	const FO3DTransportOptionSchema& Receiver = Descriptor->ReceiverOptions.OptionSchema;

	TestTrue(TEXT("Sender lists Delivery Mode"), HasKey(Sender, TEXT("delivery_mode")));
	TestTrue(TEXT("Sender lists Queue Capacity"), HasKey(Sender, TEXT("queue_bytes")));
	TestFalse(TEXT("Receiver does not list Delivery Mode"), HasKey(Receiver, TEXT("delivery_mode")));
	TestFalse(TEXT("Receiver does not list Queue Capacity"), HasKey(Receiver, TEXT("queue_bytes")));
	for (const TCHAR* Shared : { TEXT("relay_url"), TEXT("track_namespace"), TEXT("track_name") })
	{
		TestTrue(*FString::Printf(TEXT("Both sides list %s"), Shared), HasKey(Sender, Shared) && HasKey(Receiver, Shared));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_MOQ
