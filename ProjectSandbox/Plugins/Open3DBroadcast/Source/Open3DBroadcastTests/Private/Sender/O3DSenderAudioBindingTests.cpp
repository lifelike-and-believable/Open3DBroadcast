// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// FO3DSenderAudioBinding (WP-A3 step 7, SND-22): the audio configs built from the sender's
// properties, finding the capture component and handing it the transport's sink, reached through
// FO3DSenderAudioBindingProbe. The capture component is never registered and runs in Mix mode, so
// binding a sink opens no device and starts no submix tap.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "O3DAudioInputDevices.h"
#include "O3DSenderAudioCaptureComponent.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DSenderInterface.h"
#include "UObject/Package.h"

namespace O3DSenderAudioBindingTests
{
	/** Installs a fake device list; restores the platform enumeration and refills the cache when it ends. */
	struct FScopedFakeDevices
	{
		explicit FScopedFakeDevices(const TArray<FString>& Names)
		{
			FO3DAudioInputDevices::Get().SetEnumeratorForTesting([Names]() { return Names; });
			FO3DAudioInputDevices::Get().Refresh();
		}
		~FScopedFakeDevices()
		{
			FO3DAudioInputDevices::Get().SetEnumeratorForTesting(FO3DAudioInputDevices::FEnumerator());
			FO3DAudioInputDevices::Get().Refresh();
		}
	};

	class FDiscardingSink final : public IO3DSenderAudioSink
	{
	public:
		virtual bool SubmitPcm(const FString&, const float*, int32, int32, int32, double) override { return true; }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioBindingConfigTest, "Open3DBroadcast.Sender.AudioBinding.BuildsConfigsFromProperties", O3DB_TEST_FLAGS)
bool FO3DSenderAudioBindingConfigTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderAudioBindingTests;
	FScopedFakeDevices Devices({ TEXT("Desk Mic"), TEXT("Headset") });

	FO3DSenderAudioBindingProbe Binding;
	Binding.CaptureConfig.SampleRate = 44100;
	Binding.CaptureConfig.NumChannels = 2;
	Binding.CaptureConfig.BitrateKbps = 96;
	Binding.CaptureConfig.DeviceIndex = 7;
	Binding.Codec = FName(TEXT("PCM16"));

	// Mix: the game submix; the device index is left as configured.
	Binding.Mode = EO3DSenderCaptureMode::Mix;
	Binding.InputDevice = FName(TEXT("Headset"));
	TestTrue(TEXT("Mix taps the game submix"), Binding.BuildCaptureConfig().Source == EO3DSenderAudioSource::GameSubmix);
	TestEqual(TEXT("Mix keeps the configured device index"), Binding.BuildCaptureConfig().DeviceIndex, 7);
	FO3DTransportAudioConfig Transport = Binding.BuildTransportConfig();
	TestTrue(TEXT("Audio on"), Transport.bEnableAudio);
	TestEqual(TEXT("Mode string"), Transport.Mode, FString(TEXT("mix")));
	TestTrue(TEXT("No input device in Mix"), Transport.InputDevice.IsEmpty());
	TestEqual(TEXT("Sample rate"), Transport.SampleRate, 44100);
	TestEqual(TEXT("Channels"), Transport.NumChannels, 2);
	TestEqual(TEXT("Bitrate"), Transport.BitrateKbps, 96);

	// Input: the microphone, with the index of the named device in the cached list.
	Binding.Mode = EO3DSenderCaptureMode::Input;
	TestTrue(TEXT("Input uses the microphone"), Binding.BuildCaptureConfig().Source == EO3DSenderAudioSource::Microphone);
	TestEqual(TEXT("Input resolves the device from the cache"), Binding.BuildCaptureConfig().DeviceIndex, 1);
	Transport = Binding.BuildTransportConfig();
	TestEqual(TEXT("Mode string"), Transport.Mode, FString(TEXT("input")));
	TestEqual(TEXT("The device name"), Transport.InputDevice, FString(TEXT("Headset")));
	TestEqual(TEXT("And its index"), Transport.AdvancedParams.FindRef(TEXT("device_index")), FString(TEXT("1")));
	TestTrue(TEXT("Gains are passed on"), Transport.AdvancedParams.Contains(TEXT("game_gain")) && Transport.AdvancedParams.Contains(TEXT("mic_gain")));
	TestFalse(TEXT("No submix without one"), Transport.AdvancedParams.Contains(TEXT("submix")));
	TestTrue(TEXT("A known codec is named"), !Transport.Codec.IsEmpty() && Transport.AdvancedParams.FindRef(TEXT("codec")) == Transport.Codec);

	// SyncSource keeps the property in step with the mode.
	Binding.CaptureConfig.Source = EO3DSenderAudioSource::GameSubmix;
	Binding.CaptureConfig.DeviceIndex = -1;
	Binding.SyncSource();
	TestTrue(TEXT("SyncSource sets the source"), Binding.CaptureConfig.Source == EO3DSenderAudioSource::Microphone);
	TestEqual(TEXT("And the device index"), Binding.CaptureConfig.DeviceIndex, 1);

	// Audio off: the transport is told only that.
	Binding.bEnableAudio = false;
	Transport = Binding.BuildTransportConfig();
	TestFalse(TEXT("Audio off"), Transport.bEnableAudio);
	TestEqual(TEXT("Nothing else"), Transport.AdvancedParams.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioBindingSinkTest, "Open3DBroadcast.Sender.AudioBinding.FindsCaptureAndBindsSink", O3DB_TEST_FLAGS)
bool FO3DSenderAudioBindingSinkTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderAudioBindingTests;

	TestNull(TEXT("No owner, no capture component"), FO3DSenderAudioBindingProbe::FindOrCreateCaptureComponent(nullptr, nullptr));

	UO3DSenderAudioCaptureComponent* Capture = NewObject<UO3DSenderAudioCaptureComponent>(GetTransientPackage());
	TestTrue(TEXT("A usable capture component is kept"), FO3DSenderAudioBindingProbe::FindOrCreateCaptureComponent(nullptr, Capture) == Capture);

	FO3DSenderAudioBindingProbe Binding;
	Binding.AttachSink(*Capture, nullptr, TEXT("Hero"), 10.0);
	TestEqual(TEXT("No sink: the warning is logged and timed"), Binding.GetLastSinkWarningTime(), 10.0);
	Binding.AttachSink(*Capture, nullptr, TEXT("Hero"), 11.0);
	TestEqual(TEXT("Not again within 2 s"), Binding.GetLastSinkWarningTime(), 10.0);
	Binding.AttachSink(*Capture, nullptr, TEXT("Hero"), 12.5);
	TestEqual(TEXT("Again after 2 s"), Binding.GetLastSinkWarningTime(), 12.5);

	const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink = MakeShared<FDiscardingSink, ESPMode::ThreadSafe>();
	Binding.AttachSink(*Capture, Sink, TEXT("Hero"), 13.0);
	TestEqual(TEXT("A sink clears the warning"), Binding.GetLastSinkWarningTime(), 0.0);

	Binding.AttachSink(*Capture, nullptr, TEXT("Hero"), 14.0);
	Binding.Detach(Capture);
	TestEqual(TEXT("Detach resets the warning"), Binding.GetLastSinkWarningTime(), 0.0);
	Binding.Detach(nullptr);
	return true;
}

// WP-R1 (mid-project review SR-2): receivers drop audio at a rate they do not accept, so the
// sender sends at the nearest rate they do (and, with Opus, one Opus can encode), and says so.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioBindingSampleRateTest, "Open3DBroadcast.Sender.AudioBinding.SendsOnlyRatesReceiversPlay", O3DB_TEST_FLAGS)
bool FO3DSenderAudioBindingSampleRateTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("not a rate receivers play"), EAutomationExpectedMessageFlags::Contains, 0);

	FO3DSenderAudioBindingProbe Binding;
	Binding.Mode = EO3DSenderCaptureMode::Mix;
	Binding.Codec = FName(TEXT("PCM16"));

	Binding.CaptureConfig.SampleRate = 44100;
	TestEqual(TEXT("A supported rate is kept"), Binding.BuildTransportConfig().SampleRate, 44100);

	Binding.CaptureConfig.SampleRate = 30000;
	TestEqual(TEXT("An unsupported rate is captured at the nearest supported one"), Binding.BuildCaptureConfig().SampleRate, 32000);
	TestEqual(TEXT("And sent at it"), Binding.BuildTransportConfig().SampleRate, 32000);

	Binding.Codec = FName(TEXT("Opus"));
	Binding.CaptureConfig.SampleRate = 44100;
	TestEqual(TEXT("With Opus, the nearest rate Opus encodes"), Binding.BuildTransportConfig().SampleRate, 48000);
	Binding.CaptureConfig.SampleRate = 22050;
	TestEqual(TEXT("Below 24 kHz too"), Binding.BuildTransportConfig().SampleRate, 24000);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
