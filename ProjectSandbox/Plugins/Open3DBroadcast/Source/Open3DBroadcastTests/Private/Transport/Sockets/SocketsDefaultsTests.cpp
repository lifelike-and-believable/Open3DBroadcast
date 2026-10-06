// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U6 (TRB-28): the defaults a TCP or UDP panel shows are the ones its configure function uses
// when the option is empty. Both now come from one set of constants; this test keeps them from
// drifting apart again. The registered descriptors, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Misc/AutomationTest.h"

#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsDefaultsAgreeTest, "Open3DBroadcast.Transport.Sockets.PanelDefaultsMatchConfigureDefaults", O3DB_TEST_FLAGS)
bool FO3DSocketsDefaultsAgreeTest::RunTest(const FString& Parameters)
{
	const TMap<FString, FString> NoOptions;
	int32 Compared = 0;

	for (const TCHAR* Transport : { TEXT("TCP"), TEXT("UDP") })
	{
		const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(Transport);
		if (!TestTrue(*FString::Printf(TEXT("%s is registered"), Transport), Descriptor.IsValid()))
		{
			continue;
		}
		for (const bool bSender : { true, false })
		{
			const TCHAR* Side = bSender ? TEXT("sender") : TEXT("receiver");
			const FO3DTransportOptionSchema& Schema = bSender ? Descriptor->SenderOptions.OptionSchema : Descriptor->ReceiverOptions.OptionSchema;
			FO3DTransportConfig Config(Transport, bSender ? EO3DTransportRole::Sender : EO3DTransportRole::Receiver);
			if (bSender)
			{
				Descriptor->ConfigureSender(FO3DTransportOptionsView(NoOptions, &Schema), Config);
			}
			else
			{
				Descriptor->ConfigureReceiver(FO3DTransportOptionsView(NoOptions, &Schema), Config);
			}

			for (const FO3DTransportOptionField& Field : Schema)
			{
				const FString* Written = Config.AdvancedParams.Find(Field.Key);
				if (Field.Default.IsEmpty() || Written == nullptr)
				{
					continue;
				}
				++Compared;
				TestTrue(*FString::Printf(TEXT("%s %s '%s': panel default '%s' equals the configured default '%s'"), Transport, Side, *Field.Key, *Field.Default, **Written),
					Written->Equals(Field.Default, ESearchCase::IgnoreCase));
			}
		}
	}
	TestTrue(TEXT("Some defaults were compared"), Compared >= 8);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
