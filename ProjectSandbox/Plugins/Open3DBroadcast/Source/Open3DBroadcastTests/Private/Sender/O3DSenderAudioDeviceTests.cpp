// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A2d (ADR 0008 item 8, SND-18): audio capture devices are enumerated once per StartCapture
// that captures from an input device, into a cache that the device pickers and the name-to-index
// lookups read; the device is opened once per start. A fake enumeration stands in for the platform
// (the CI runners may have no capture device). Opening the device goes through the engine, but
// Run-AutomationTests.ps1 starts the editor with -NoSound, so FApp::CanEverRenderAudio() is false
// and Audio::FAudioCapture uses its null device, whose open fails without logging; the component
// then logs "Failed to open mic stream", which these tests allow. Same on every host. The open
// attempts are what they count.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "O3DAudioInputDevices.h"
#include "O3DSenderAudioCaptureComponent.h"
#include "O3DSenderComponent.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DSenderInterface.h"
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

	/** Accepts and drops everything. */
	class FDiscardingSenderAudioSink final : public IO3DSenderAudioSink
	{
	public:
		virtual bool SubmitPcm(const FString&, const float*, int32, int32, int32, double) override { return true; }
	};

	/** A standalone game world with its own world context, destroyed when the scope ends (pitfall 26). */
	class FAudioDeviceTestWorld
	{
	public:
		FAudioDeviceTestWorld()
		{
			if (GEngine == nullptr)
			{
				return;
			}
			World = UWorld::CreateWorld(EWorldType::Game, false);
			if (World == nullptr)
			{
				return;
			}
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
		}

		~FAudioDeviceTestWorld()
		{
			if (World != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
				World->RemoveFromRoot();
			}
		}

		FAudioDeviceTestWorld(const FAudioDeviceTestWorld&) = delete;
		FAudioDeviceTestWorld& operator=(const FAudioDeviceTestWorld&) = delete;

		UWorld* Get() const { return World; }

	private:
		UWorld* World = nullptr;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderAudioDeviceSinkBindTest, "Open3DBroadcast.Sender.AudioDevices.SinkBindOpensAtMostOnce", O3DB_TEST_FLAGS)
bool FO3DSenderAudioDeviceSinkBindTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderAudioDeviceTests;

	// Every open fails here (-NoSound: the engine's null capture device), the worst case for
	// retries: a failed open must not be tried again until the capture restarts.
	AddExpectedMessage(TEXT("Failed to (open|start) mic stream"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);

	FAudioDeviceTestWorld TestWorld;
	UWorld* World = TestWorld.Get();
	if (!TestNotNull(TEXT("A standalone game world"), World))
	{
		return false;
	}
	AActor* Actor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("An actor"), Actor))
	{
		return false;
	}

	UO3DSenderAudioCaptureComponent* Capture = NewObject<UO3DSenderAudioCaptureComponent>(Actor);
	Capture->CaptureMode = EO3DSenderCaptureMode::Input;
	Capture->RegisterComponent();
	// No game mode: dispatch BeginPlay here (pitfall 26).
	Actor->DispatchBeginPlay();
	if (!TestTrue(TEXT("The capture component has begun play"), Capture->HasBegunPlay()))
	{
		return false;
	}
	TestEqual(TEXT("BeginPlay without a sink does not open the device"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 0);

	// What the sender component does on a start with a transport: configure (opens), then bind.
	TSharedRef<FDiscardingSenderAudioSink, ESPMode::ThreadSafe> Sink = MakeShared<FDiscardingSenderAudioSink, ESPMode::ThreadSafe>();
	Capture->StartCaptureWithMode(EO3DSenderCaptureMode::Input);
	TestEqual(TEXT("The start opens the device once"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 1);
	Capture->SetAudioSink(Sink, TEXT("hero"));
	TestEqual(TEXT("Binding the sink after the start does not open it again"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 1);
	Capture->SetAudioSink(nullptr, FString());
	Capture->SetAudioSink(Sink, TEXT("hero"));
	TestEqual(TEXT("Nor does rebinding it"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 1);

	// A restart may try again, once.
	Capture->StartCaptureWithMode(EO3DSenderCaptureMode::Input);
	TestEqual(TEXT("The next start opens once more"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Capture), 2);

	// Standalone use: no start, a sink bound after BeginPlay opens the device, once.
	UO3DSenderAudioCaptureComponent* Standalone = NewObject<UO3DSenderAudioCaptureComponent>(Actor);
	Standalone->CaptureMode = EO3DSenderCaptureMode::Input;
	Standalone->RegisterComponent(); // the actor has begun play, so this runs BeginPlay
	TestTrue(TEXT("The standalone component has begun play"), Standalone->HasBegunPlay());
	TestEqual(TEXT("Standalone: nothing opened without a sink"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Standalone), 0);
	Standalone->SetAudioSink(Sink, TEXT("solo"));
	TestEqual(TEXT("Standalone: binding a sink opens the device"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Standalone), 1);
	Standalone->SetAudioSink(nullptr, FString());
	Standalone->SetAudioSink(Sink, TEXT("solo"));
	TestEqual(TEXT("Standalone: a failed open is not retried on rebind"), FO3DSenderAudioCaptureTestAccess::GetNumMicOpenAttempts(*Standalone), 1);

	Capture->SetAudioSink(nullptr, FString());
	Standalone->SetAudioSink(nullptr, FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
