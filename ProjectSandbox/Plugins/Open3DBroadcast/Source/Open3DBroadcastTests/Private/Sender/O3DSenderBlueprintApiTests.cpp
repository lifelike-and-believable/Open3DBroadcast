// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U2 (SND-26, UX-3, DOC-4): the sender component's Blueprint API.
// - On Connection State Changed follows the transport's state changes, which its state callback
//   posts from any thread and the component announces on the game thread: nothing between two
//   ticks is lost, and a Failed state brings On Sender Error with the transport's reason.
// - On Capture Started / On Capture Stopped bracket a capture run; a transport that cannot start
//   leaves the state Failed (sticky until the next start) and reports why.
// - Capture without a transport and without a C++ frame consumer warns once per start.
// - The Blueprint surface the user guide documents exists, TransportName is read-only in
//   Blueprint, and Start/Stop Capture are no longer editor buttons.
// Control-only capture with no world and a test-only fake transport: no mesh, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "O3DSenderComponent.h"
#include "O3DSenderTestListener.h"
#include "O3DTestFakes.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DSenderBlueprintApiTest
{
	/** Registers a sender-only fake transport for a unique name and keeps the instance it creates. */
	struct FScopedFakeSenderTransport
	{
		FName Name;
		TSharedRef<TSharedPtr<FO3DFakeSender, ESPMode::ThreadSafe>> Created = MakeShared<TSharedPtr<FO3DFakeSender, ESPMode::ThreadSafe>>();
		FO3DTransportRegistration Registration;

		FScopedFakeSenderTransport()
			: Name(*O3DTests::MakeUniqueName(TEXT("O3DBlueprintApiSender")))
		{
			const TSharedRef<TSharedPtr<FO3DFakeSender, ESPMode::ThreadSafe>> Holder = Created;
			FO3DTransportDescriptor Descriptor;
			Descriptor.Name = Name;
			Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
			Descriptor.CreateSender = [Holder]() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>
			{
				*Holder = MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>();
				return *Holder;
			};
			Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
		}

		~FScopedFakeSenderTransport()
		{
			// The test's own reference goes first: the registry reports instances still alive when
			// their transport unregisters (ADR 0007 item 5).
			Created->Reset();
			Registration.Reset();
		}

		FScopedFakeSenderTransport(const FScopedFakeSenderTransport&) = delete;
		FScopedFakeSenderTransport& operator=(const FScopedFakeSenderTransport&) = delete;
	};

	/** A component that captures control only, with its Blueprint events bound to Listener. */
	UO3DSenderComponent* MakeComponent(UO3DSenderTestListener* Listener)
	{
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
		Component->bEnableAudio = false;
		Component->bAllowControlOnly = true;
		Component->OnConnectionStateChanged.AddDynamic(Listener, &UO3DSenderTestListener::OnStateChanged);
		Component->OnCaptureStarted.AddDynamic(Listener, &UO3DSenderTestListener::OnStarted);
		Component->OnCaptureStopped.AddDynamic(Listener, &UO3DSenderTestListener::OnStopped);
		Component->OnSenderError.AddDynamic(Listener, &UO3DSenderTestListener::OnError);
		return Component;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderBlueprintStateEventsTest, "Open3DBroadcast.Sender.BlueprintApi.StateEventsFollowTransport", O3DB_TEST_FLAGS)
bool FO3DSenderBlueprintStateEventsTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderBlueprintApiTest;
	FScopedFakeSenderTransport Transport;
	UO3DSenderTestListener* Listener = NewObject<UO3DSenderTestListener>(GetTransientPackage());
	UO3DSenderComponent* Component = MakeComponent(Listener);
	Component->bAutoCreateTransport = true;
	Component->SetTransportName(Transport.Name);

	Component->StartCapture();
	if (!TestTrue(TEXT("Capturing"), Component->IsCapturing()) || !TestTrue(TEXT("The fake transport was created"), Transport.Created->IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("On Capture Started once"), Listener->Started, 1);
	TestTrue(TEXT("The start's Connected is announced"), Listener->States.Contains(EO3DBroadcastConnectionState::Connected));
	TestEqual(TEXT("Get Connection State: Connected"), Component->GetConnectionState(), EO3DBroadcastConnectionState::Connected);
	TestEqual(TEXT("No error on a good start"), Listener->Errors.Num(), 0);

	// Two changes between ticks, on the calling thread as a worker would make them: both are seen.
	Listener->States.Reset();
	(*Transport.Created)->SimulateConnectionState(EO3DConnectionState::Failed, FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, TEXT("relay refused")));
	(*Transport.Created)->SimulateConnectionState(EO3DConnectionState::Reconnecting);
	TestEqual(TEXT("Nothing is announced before the game thread drains"), Listener->States.Num(), 0);
	FO3DSenderComponentTestAccess::DrainConnectionState(*Component);
	TestTrue(TEXT("Failed, then Reconnecting"), Listener->States == TArray<EO3DBroadcastConnectionState>({ EO3DBroadcastConnectionState::Failed, EO3DBroadcastConnectionState::Reconnecting }));
	TestTrue(TEXT("On Sender Error carries the transport's reason"), Listener->Errors.Num() == 1 && Listener->Errors[0].Contains(TEXT("relay refused")));
	TestEqual(TEXT("Get Connection State: Reconnecting"), Component->GetConnectionState(), EO3DBroadcastConnectionState::Reconnecting);
	TestEqual(TEXT("Stats carry the state"), Component->GetTransportStats().State, EO3DBroadcastConnectionState::Reconnecting);

	FO3DSenderComponentTestAccess::DrainConnectionState(*Component);
	TestEqual(TEXT("A drain with nothing new announces nothing"), Listener->States.Num(), 2);

	Component->StopCapture();
	TestEqual(TEXT("On Capture Stopped once"), Listener->Stopped, 1);
	TestEqual(TEXT("Stopped: Idle announced last"), Listener->States.Last(), EO3DBroadcastConnectionState::Idle);
	TestEqual(TEXT("Get Connection State: Idle"), Component->GetConnectionState(), EO3DBroadcastConnectionState::Idle);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderBlueprintStartFailuresTest, "Open3DBroadcast.Sender.BlueprintApi.StartFailuresAreReported", O3DB_TEST_FLAGS)
bool FO3DSenderBlueprintStartFailuresTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderBlueprintApiTest;

	// A transport that cannot start: Failed stays until the next start, with the reason.
	{
		UO3DSenderTestListener* Listener = NewObject<UO3DSenderTestListener>(GetTransientPackage());
		UO3DSenderComponent* Component = MakeComponent(Listener);
		Component->bAutoCreateTransport = true;
		Component->SetTransportName(FName(*O3DTests::MakeUniqueName(TEXT("O3DNoSuchTransport"))));

		AddExpectedError(TEXT("No sender registered for transport"), EAutomationExpectedMessageFlags::Contains, 1);
		Component->StartCapture();
		TestTrue(TEXT("Capture still runs without its transport"), Component->IsCapturing());
		TestEqual(TEXT("Get Connection State: Failed"), Component->GetConnectionState(), EO3DBroadcastConnectionState::Failed);
		TestTrue(TEXT("Failed announced"), Listener->States == TArray<EO3DBroadcastConnectionState>({ EO3DBroadcastConnectionState::Failed }));
		TestTrue(TEXT("On Sender Error says why"), Listener->Errors.Num() == 1 && Listener->Errors[0].Contains(TEXT("No sender is registered")));

		FO3DSenderComponentTestAccess::DrainConnectionState(*Component);
		TestEqual(TEXT("Failed is sticky across ticks"), Component->GetConnectionState(), EO3DBroadcastConnectionState::Failed);

		Component->StopCapture();
		TestEqual(TEXT("Stopping returns to Idle"), Component->GetConnectionState(), EO3DBroadcastConnectionState::Idle);
	}

	// Nothing to capture: the start error reaches On Sender Error, and capture never starts.
	{
		UO3DSenderTestListener* Listener = NewObject<UO3DSenderTestListener>(GetTransientPackage());
		UO3DSenderComponent* Component = MakeComponent(Listener);
		Component->bAllowControlOnly = false;

		AddExpectedError(TEXT("Sender capture not started"), EAutomationExpectedMessageFlags::Contains, 1);
		Component->StartCapture();
		TestFalse(TEXT("Not capturing"), Component->IsCapturing());
		TestTrue(TEXT("On Sender Error carries the start error"), Listener->Errors.Num() == 1 && Listener->Errors[0] == Component->GetLastStartCaptureError());
		TestEqual(TEXT("No On Capture Started"), Listener->Started, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderBlueprintNoTransportWarningTest, "Open3DBroadcast.Sender.BlueprintApi.WarnsWithoutTransport", O3DB_TEST_FLAGS)
bool FO3DSenderBlueprintNoTransportWarningTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderBlueprintApiTest;
	UO3DSenderTestListener* Listener = NewObject<UO3DSenderTestListener>(GetTransientPackage());
	UO3DSenderComponent* Component = MakeComponent(Listener);
	TestFalse(TEXT("Auto Create Transport is off by default"), Component->bAutoCreateTransport);

	AddExpectedError(TEXT("Auto Create Transport is off"), EAutomationExpectedMessageFlags::Contains, 1);
	Component->StartCapture();
	TestTrue(TEXT("Capture starts"), Component->IsCapturing());
	TestEqual(TEXT("Idle: no transport"), Component->GetConnectionState(), EO3DBroadcastConnectionState::Idle);
	TestEqual(TEXT("Not an error"), Listener->Errors.Num(), 0);
	Component->StopCapture();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderBlueprintSurfaceTest, "Open3DBroadcast.Sender.BlueprintApi.Surface", O3DB_TEST_FLAGS)
bool FO3DSenderBlueprintSurfaceTest::RunTest(const FString& Parameters)
{
	const UClass* Class = UO3DSenderComponent::StaticClass();

	const TCHAR* Callable[] = { TEXT("StartCapture"), TEXT("StopCapture"), TEXT("SetTransportName"), TEXT("SetTransportOption"), TEXT("ClearTransportOptions") };
	for (const TCHAR* Name : Callable)
	{
		const UFunction* Function = Class->FindFunctionByName(Name);
		TestTrue(FString::Printf(TEXT("%s is BlueprintCallable"), Name), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}
	const TCHAR* Pure[] = { TEXT("IsCapturing"), TEXT("GetConnectionState"), TEXT("GetTransportStats"), TEXT("GetTransportName"), TEXT("GetTransportOption") };
	for (const TCHAR* Name : Pure)
	{
		const UFunction* Function = Class->FindFunctionByName(Name);
		TestTrue(FString::Printf(TEXT("%s is BlueprintPure"), Name), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
	}
#if WITH_EDITOR
	TestFalse(TEXT("Start Capture is not an editor button"), Class->FindFunctionByName(TEXT("StartCapture"))->HasMetaData(TEXT("CallInEditor")));
	TestFalse(TEXT("Stop Capture is not an editor button"), Class->FindFunctionByName(TEXT("StopCapture"))->HasMetaData(TEXT("CallInEditor")));
#endif

	const TCHAR* Events[] = { TEXT("OnConnectionStateChanged"), TEXT("OnCaptureStarted"), TEXT("OnCaptureStopped"), TEXT("OnSenderError") };
	for (const TCHAR* Name : Events)
	{
		const FProperty* Property = Class->FindPropertyByName(Name);
		TestTrue(FString::Printf(TEXT("%s is BlueprintAssignable"), Name), Property && Property->HasAnyPropertyFlags(CPF_BlueprintAssignable));
	}

	const FProperty* TransportName = Class->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName));
	TestTrue(TEXT("TransportName is read-only in Blueprint"), TransportName && TransportName->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
