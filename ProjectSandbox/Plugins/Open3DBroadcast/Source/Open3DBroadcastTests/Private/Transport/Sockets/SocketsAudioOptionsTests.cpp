// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U6 (TRB-25): TCP and UDP carry audio on the data socket, in the unified envelope. Nothing
// read the audio.port, audio.host and audio.bind options the configure functions wrote, and the
// Port tooltip claimed audio used the next port. The registered TCP and UDP descriptors, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS

#include "Misc/AutomationTest.h"

#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSocketsNoAudioPortTest, "Open3DBroadcast.Transport.Sockets.AudioSharesTheDataSocket", O3DB_TEST_FLAGS)
bool FO3DSocketsNoAudioPortTest::RunTest(const FString& Parameters)
{
	const TCHAR* const AudioKeys[] = { TEXT("audio.port"), TEXT("audio.host"), TEXT("audio.bind") };
	const TMap<FString, FString> NoOptions;

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
			Config.Audio.bEnableAudio = true;
			if (bSender)
			{
				Descriptor->ConfigureSender(FO3DTransportOptionsView(NoOptions, &Schema), Config);
			}
			else
			{
				Descriptor->ConfigureReceiver(FO3DTransportOptionsView(NoOptions, &Schema), Config);
			}

			for (const TCHAR* Key : AudioKeys)
			{
				TestFalse(*FString::Printf(TEXT("%s %s with audio writes no %s"), Transport, Side, Key), Config.AdvancedParams.Contains(Key));
			}

			const FO3DTransportOptionField* Port = Schema.FindByPredicate([](const FO3DTransportOptionField& Field) { return Field.Key == TEXT("port"); });
			if (TestNotNull(*FString::Printf(TEXT("%s %s has a Port field"), Transport, Side), Port))
			{
				const FString Tooltip = Port->Tooltip.ToString();
				TestFalse(*FString::Printf(TEXT("%s %s Port tooltip does not send audio to another port"), Transport, Side), Tooltip.Contains(TEXT("next port")) || Tooltip.Contains(TEXT("audio.port")));
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && O3D_WITH_TRANSPORT_SOCKETS
