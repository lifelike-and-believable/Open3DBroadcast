// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A2d (ADR 0008 item 8, SND-18): audio capture devices are enumerated once per StartCapture
// that captures from an input device, into a cache that the device pickers and the name-to-index
// lookups read; the device is opened once per start. A fake enumeration stands in for the platform
// (the CI runners may have no capture device). Opening the device is real: on a host without one it
// fails with a warning, which these tests allow; the open attempts are what they count.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "O3DAudioInputDevices.h"
#include "O3DSenderAudioCaptureComponent.h"
#include "O3DSenderComponent.h"
#include "Testing/O3DSenderTesting.h"
#include "UObject/Package.h"

namespace O3DSenderAudioDeviceTests
{
	/** Installs a fake device list; restores the platform enumeration and refills the cache when it ends. */
	class FScopedFakeDevices
	{
	public:
		explicit FScopedFakeDevices(TArray<FString> InNames)
			: Names(MakeShared<TArray<FString>>(MoveTemp(InNames)))
		{
			TSharedRef<TArray<FString>> Shared = Names;
			FO3DAudioInputDevices::Get().SetEnumeratorForTesting([Shared]() { return *Shared; });
		}

		~FScopedFakeDevices()
		{
			FO3DAudioInputDevices& Devices = FO3DAudioInputDevices::Get();
			Devices.SetEnumeratorForTesting(FO3DAudioInputDevices::FEnumerator());
			// Leave the real list behind, not the fake one, for the editor's device pickers.
			Devices.Refresh();
		}

		FScopedFakeDevices(const FScopedFakeDevices&) = delete;
		FScopedFakeDevices& operator=(const FScopedFakeDevices&) = delete;

		/** What the next enumeration returns (a device plugged in or removed). */
		void Set(TArray<FString> InNames) { *Names = MoveTemp(InNames); }

	private:
		TSharedRef<TArray<FString>> Names;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioDeviceLookupsTest, "Open3DBroadcast.Sender.AudioDevices.LookupsReadTheCache", O3DB_TEST_FLAGS)
bool FO3DSenderAudioDeviceLookupsTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderAudioDeviceTests;

	FScopedFakeDevices Fake({ TEXT("Fake Mic A"), TEXT("Fake Mic B") });
	FO3DAudioInputDevices& Devices = FO3DAudioInputDevices::Get();
	Devices.Refresh();
	const int32 Enumerations = Devices.GetEnumerationCount();

	TestTrue(TEXT("The cache is filled"), Devices.HasEnumerated());
	TestEqual(TEXT("Two cached devices"), Devices.GetNames().Num(), 2);
	TestEqual(TEXT("Names resolve case-insensitively"), Devices.FindIndex(FName(TEXT("fake mic b"))), 1);
	TestEqual(TEXT("None is the default device"), Devices.FindIndex(NAME_None), -1);
	TestEqual(TEXT("An unknown name is the default device"), Devices.FindIndex(FName(TEXT("Missing Mic"))), -1);

	// Creating components and asking for the picker options and indices reads the cache only.
	UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Sender->bEnableAudio = true;
	Sender->AudioCaptureMode = EO3DSenderCaptureMode::Input;
	Sender->AudioInputDevice = FName(TEXT("Fake Mic B"));
	UO3DSenderAudioCaptureComponent* Capture = NewObject<UO3DSenderAudioCaptureComponent>(GetTransientPackage());

	const TArray<FName> SenderOptions = Sender->GetAvailableAudioInputDeviceOptions();
	const TArray<FName> CaptureOptions = Capture->GetAvailableInputDeviceOptions();
	TestEqual(TEXT("The sender's picker lists the cached devices"), SenderOptions.Num(), 2);
	TestTrue(TEXT("In the platform's order"), SenderOptions.Num() == 2 && SenderOptions[1] == FName(TEXT("Fake Mic B")));
	TestEqual(TEXT("The capture component's picker lists them too"), CaptureOptions.Num(), 2);
	TestEqual(TEXT("The capture config resolves the device from the cache"), FO3DSenderComponentTestAccess::BuildAudioCaptureConfig(*Sender).DeviceIndex, 1);
	TestEqual(TEXT("None of that enumerated"), Devices.GetEnumerationCount(), Enumerations);

	// A device removed: the cache keeps the old list until the next enumeration.
	Fake.Set({ TEXT("Fake Mic B") });
	TestEqual(TEXT("Lookups still see the cached list"), FO3DSenderComponentTestAccess::BuildAudioCaptureConfig(*Sender).DeviceIndex, 1);
	UO3DSenderComponent::RefreshAudioInputDevices();
	TestEqual(TEXT("RefreshAudioInputDevices enumerates once"), Devices.GetEnumerationCount(), Enumerations + 1);
	TestEqual(TEXT("And the index follows the new list"), FO3DSenderComponentTestAccess::BuildAudioCaptureConfig(*Sender).DeviceIndex, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioDeviceStartTest, "Open3DBroadcast.Sender.AudioDevices.StartEnumeratesAndOpensOnce", O3DB_TEST_FLAGS)
bool FO3DSenderAudioDeviceStartTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderAudioDeviceTests;

	// Audio without a mesh: each StartCapture warns once that no mesh is set (pitfall 22). A host
	// without a capture device cannot open it; that warning is allowed in any number.
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 3);
	AddExpectedMessage(TEXT("Failed to (open|start) mic stream"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

	FScopedFakeDevices Fake({ TEXT("Fake Mic A"), TEXT("Fake Mic B") });
	FO3DAudioInputDevices& Devices = FO3DAudioInputDevices::Get();

	// No world: StartCapture skips the game-world check and there is no owner to create a capture
	// component on, so the test hands one over. No transport (bAutoCreateTransport is false), so
	// no sink is bound and the opened stream is never started.
	UO3DSenderComponent* Sender = NewObject<UO3DSenderComponent>(GetTransientPackage());
	UO3DSenderAudioCaptureComponent* Capture = NewObject<UO3DSenderAudioCaptureComponent>(GetTransientPackage());
	Sender->AddToRoot();
	Capture->AddToRoot();
	ON_SCOPE_EXIT
	{
		Sender->StopCapture();
		// Mix mode closes the device the Input starts opened.
		Capture->StartCaptureWithMode(EO3DSenderCaptureMode::Mix);
		FO3DSenderComponentTestAccess::SetAudioCaptureComponent(*Sender, nullptr);
		Capture->RemoveFromRoot();
		Sender->RemoveFromRoot();
	};
	Sender->bEnableAudio = true;
	Sender->AudioCaptureMode = EO3DSenderCaptureMode::Input;
	Sender->AudioInputDevice = FName(TEXT("Fake Mic B"));
	FO3DSenderComponentTestAccess::SetAudioCaptureComponent(*Sender, Capture);

	const int32 Enumerations = Devices.GetEnumerationCount();
	Sender->StartCapture();
	if (!TestTrue(TEXT("Capture started (audio only)"), Sender->IsCapturing()))
	{
		return false;
	}
	TestEqual(TEXT("StartCapture enumerates the devices once"), Devices.GetEnumerationCount(), Enumerations + 1);
	TestEqual(TEXT("And opens the device once"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 1);
	TestEqual(TEXT("The capture component got the resolved index"), Capture->Config.DeviceIndex, 1);
	Sender->StopCapture();

	// The device list changed between starts: the next start sees it.
	Fake.Set({ TEXT("Fake Mic B") });
	Sender->StartCapture();
	TestEqual(TEXT("A second start enumerates once more"), Devices.GetEnumerationCount(), Enumerations + 2);
	TestEqual(TEXT("And opens the device once more"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 2);
	TestEqual(TEXT("With the index from the new list"), Capture->Config.DeviceIndex, 0);
	Sender->StopCapture();

	// Mix mode taps a submix: no device to enumerate or open.
	Sender->AudioCaptureMode = EO3DSenderCaptureMode::Mix;
	Sender->StartCapture();
	TestEqual(TEXT("A Mix start does not enumerate"), Devices.GetEnumerationCount(), Enumerations + 2);
	TestEqual(TEXT("Nor open a device"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
