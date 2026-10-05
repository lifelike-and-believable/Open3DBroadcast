// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 2 (ADR 0007 item 5; SHR-13, TRF-14): the transport lifetime contract.
// - The registry tracks the instances CreateSender/CreateReceiver hand out.
// - Unregistering drains: OnTransportUnregistering fires once the name is gone, owners stop and
//   release their instances, and the registry stops what is left and reports it as a leak.
// - Unregistering with no instances, and many register/unregister cycles, are clean.
// - A factory call that races its own unregister cannot add an instance nobody drains.
// - The real owners (the sender component's transport controller and the receiver source)
//   release their instance when their transport unregisters during a live session.
// The FFI-library half (no FreeDll while an instance is alive) is in O3DFfiLibraryTests.cpp.
//
// The registry cases use a private registry (MakeShared), so they never touch the process-wide
// one the transports register with; the two owner cases use FO3DFakeTransportScope on the
// process-wide one, because that is the registry the owners subscribe to. No network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Templates/UniquePtr.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "O3DReceiverSourceSettings.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DTransportLifetimeTest
{
	using FRegistryRef = TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe>;

	FRegistryRef MakeRegistry()
	{
		return MakeShared<FO3DTransportRegistry, ESPMode::ThreadSafe>();
	}

	/** A descriptor with fake sender and receiver factories. */
	FO3DTransportDescriptor MakeFakeDescriptor(FName Name)
	{
		FO3DTransportDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
		Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>(); };
		Descriptor.CreateReceiver = []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeReceiver, ESPMode::ThreadSafe>(); };
		return Descriptor;
	}

	/** Control sink that only counts. */
	class FCountingControlSink final : public IO3DReceiverControlSink
	{
	public:
		virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& StreamId, double ReceiveTimeSec) override
		{
			(void)Payload;
			(void)StreamId;
			(void)ReceiveTimeSec;
			++Submitted;
		}

		int32 Submitted = 0;
	};

	/**
	 * Holds a running sender and receiver of one transport, the way the sender transport
	 * controller and the receiver source do, and releases them on OnTransportUnregistering.
	 */
	struct FSubscribedOwner
	{
		FSubscribedOwner(const FRegistryRef& InRegistry, FName InName)
			: Registry(InRegistry)
			, Name(InName)
		{
			Handle = Registry->OnTransportUnregistering().AddRaw(this, &FSubscribedOwner::OnUnregistering);
		}

		~FSubscribedOwner()
		{
			Registry->OnTransportUnregistering().Remove(Handle);
		}

		FSubscribedOwner(const FSubscribedOwner&) = delete;
		FSubscribedOwner& operator=(const FSubscribedOwner&) = delete;

		bool Start()
		{
			Sender = Registry->CreateSender(Name);
			Receiver = Registry->CreateReceiver(Name);
			if (!Sender.IsValid() || !Receiver.IsValid())
			{
				return false;
			}
			Receiver->SetConsumer(MakeShared<FO3DRecordingFrameConsumer>());
			Receiver->SetControlSink(MakeShared<FCountingControlSink, ESPMode::ThreadSafe>());
			FO3DTransportConfig Config;
			Config.Transport = Name;
			return Sender->Initialize(Config) && Sender->Start() && Receiver->Initialize(Config) && Receiver->Start();
		}

		void OnUnregistering(FName TransportName)
		{
			++Notifications;
			if (TransportName != Name)
			{
				return;
			}
			bNameGoneDuringNotification = !Registry->Find(Name).IsValid();
			if (Sender.IsValid())
			{
				Sender->Stop();
				SenderStopCallsAtRelease = static_cast<FO3DFakeSender*>(Sender.Get())->GetStopCalls();
				Sender.Reset();
			}
			if (Receiver.IsValid())
			{
				Receiver->SetConsumer(nullptr);
				Receiver->Stop();
				Receiver.Reset();
			}
		}

		FRegistryRef Registry;
		FName Name;
		FDelegateHandle Handle;
		TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender;
		TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver;
		int32 Notifications = 0;
		int32 SenderStopCallsAtRelease = 0;
		bool bNameGoneDuringNotification = false;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportLifetimeNoInstancesTest, "Open3DBroadcast.Shared.TransportLifetime.UnregisterWithNoInstancesIsClean", O3DB_TEST_FLAGS)
bool FO3DTransportLifetimeNoInstancesTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportLifetimeTest;

	// No AddExpectedError: an unregister with nothing alive must log no leak.
	const FRegistryRef Registry = MakeRegistry();
	const FName Name(TEXT("LifetimeTestEmpty"));

	TArray<FName> Unregistering;
	int32 ChangesAtNotification = -1;
	int32 Changes = 0;
	Registry->OnTransportsChanged().AddLambda([&Changes]() { ++Changes; });
	Registry->OnTransportUnregistering().AddLambda([&Unregistering, &ChangesAtNotification, &Changes](FName TransportName)
	{
		Unregistering.Add(TransportName);
		ChangesAtNotification = Changes;
	});

	FO3DTransportRegistration Registration = Registry->Register(MakeFakeDescriptor(Name));
	if (!TestTrue(TEXT("Registered"), Registration.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Nothing live after register"), Registry->GetNumLiveInstances(Name), 0);
	{
		// Instances that are already gone are not live.
		TestTrue(TEXT("A sender can be created"), Registry->CreateSender(Name).IsValid());
		TestTrue(TEXT("A receiver can be created"), Registry->CreateReceiver(Name).IsValid());
	}
	TestEqual(TEXT("Released instances are not counted"), Registry->GetNumLiveInstances(Name), 0);
	TestTrue(TEXT("Registering broadcast no unregistering notification"), Unregistering.Num() == 0);

	Registration.Reset();
	TestEqual(TEXT("One unregistering notification"), Unregistering.Num(), 1);
	TestTrue(TEXT("It names the transport"), Unregistering.Num() == 1 && Unregistering[0] == Name);
	TestEqual(TEXT("It fires before OnTransportsChanged for the unregister"), ChangesAtNotification, 1);
	TestEqual(TEXT("OnTransportsChanged still fires for register and unregister"), Changes, 2);
	TestEqual(TEXT("Nothing live after unregister"), Registry->GetNumLiveInstances(Name), 0);
	TestEqual(TEXT("Nothing registered"), Registry->Num(), 0);

	Registration.Reset();
	TestEqual(TEXT("A second Reset notifies nothing"), Unregistering.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportLifetimeDrainTest, "Open3DBroadcast.Shared.TransportLifetime.UnregisterStopsAndReleasesLiveSession", O3DB_TEST_FLAGS)
bool FO3DTransportLifetimeDrainTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportLifetimeTest;

	// No AddExpectedError: the owner releases everything, so no leak may be logged.
	const FRegistryRef Registry = MakeRegistry();
	const FName Name(TEXT("LifetimeTestDrain"));
	FO3DTransportRegistration Registration = Registry->Register(MakeFakeDescriptor(Name));

	FSubscribedOwner Owner(Registry, Name);
	if (!TestTrue(TEXT("Live session starts"), Owner.Start()))
	{
		return false;
	}
	const TWeakPtr<IOpen3DSender, ESPMode::ThreadSafe> WeakSender = Owner.Sender;
	const TWeakPtr<IOpen3DReceiver, ESPMode::ThreadSafe> WeakReceiver = Owner.Receiver;
	TestEqual(TEXT("The registry counts the live sender and receiver"), Registry->GetNumLiveInstances(Name), 2);

	Registration.Reset();

	TestEqual(TEXT("The owner was notified once"), Owner.Notifications, 1);
	TestTrue(TEXT("The name had left the registry when the owner was notified"), Owner.bNameGoneDuringNotification);
	TestEqual(TEXT("The owner stopped its sender"), Owner.SenderStopCallsAtRelease, 1);
	TestFalse(TEXT("The sender was released"), WeakSender.IsValid());
	TestFalse(TEXT("The receiver was released"), WeakReceiver.IsValid());
	TestEqual(TEXT("Nothing live after the drain"), Registry->GetNumLiveInstances(Name), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportLifetimeLeakTest, "Open3DBroadcast.Shared.TransportLifetime.LeakedInstanceIsStoppedAndReported", O3DB_TEST_FLAGS)
bool FO3DTransportLifetimeLeakTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportLifetimeTest;

	// The leak is logged at Error by design, naming the transport and its module; it is the
	// behaviour under test. (The pattern stops before the message's parentheses, which a
	// regular-expression match would read as a group.)
	AddExpectedError(TEXT("Transport 'LifetimeTestLeak' from module Open3DBroadcastTests unregistered with 2 live instance"), EAutomationExpectedMessageFlags::Contains, 1);

	const FRegistryRef Registry = MakeRegistry();
	const FName Name(TEXT("LifetimeTestLeak"));
	FO3DTransportRegistration Registration = Registry->Register(MakeFakeDescriptor(Name));

	// An owner that never subscribed: it keeps both instances, running, with sinks installed.
	TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Sender = Registry->CreateSender(Name);
	TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> Receiver = Registry->CreateReceiver(Name);
	if (!TestTrue(TEXT("Instances created"), Sender.IsValid() && Receiver.IsValid()))
	{
		return false;
	}
	FO3DFakeSender* FakeSender = static_cast<FO3DFakeSender*>(Sender.Get());
	FO3DFakeReceiver* FakeReceiver = static_cast<FO3DFakeReceiver*>(Receiver.Get());
	const FO3DTransportConfig Config;
	TestTrue(TEXT("Sender starts"), Sender->Initialize(Config) && Sender->Start());
	Receiver->SetConsumer(MakeShared<FO3DRecordingFrameConsumer>());
	Receiver->SetControlSink(MakeShared<FCountingControlSink, ESPMode::ThreadSafe>());
	TestTrue(TEXT("Receiver starts"), Receiver->Initialize(Config) && Receiver->Start());

	Registration.Reset();

	// The registry stopped both and took the sinks back from the receiver, but cannot release
	// what the owner still references: that is the leak it reported.
	TestEqual(TEXT("The leaked sender was stopped once"), FakeSender->GetStopCalls(), 1);
	TestTrue(TEXT("The leaked sender no longer sends (NotRunning)"), Sender->SendSerialized(FO3DSendPayload::MakeCopy(reinterpret_cast<const uint8*>("x"), 1, TEXT("s"), 0.0)) == EO3DSendResult::NotRunning);
	TestFalse(TEXT("The leaked receiver's consumer was released"), FakeReceiver->HasConsumer());
	TestFalse(TEXT("The leaked receiver's control sink was released"), FakeReceiver->HasControlSink());
	TestEqual(TEXT("Both leaked instances are still counted after the unregister"), Registry->GetNumLiveInstances(Name), 2);

	// A new registration under the same name starts clean, but the old leak stays counted.
	{
		FO3DTransportRegistration Again = Registry->Register(MakeFakeDescriptor(Name));
		TestTrue(TEXT("The name can be registered again"), Again.IsValid());
		TestEqual(TEXT("The leak is still counted under the name"), Registry->GetNumLiveInstances(Name), 2);
		Again.Reset();
	}

	Sender.Reset();
	TestEqual(TEXT("Releasing the sender lowers the count"), Registry->GetNumLiveInstances(Name), 1);
	Receiver.Reset();
	TestEqual(TEXT("Releasing the receiver ends the leak"), Registry->GetNumLiveInstances(Name), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportLifetimeCyclesTest, "Open3DBroadcast.Shared.TransportLifetime.RepeatedRegisterUnregisterCycles", O3DB_TEST_FLAGS)
bool FO3DTransportLifetimeCyclesTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportLifetimeTest;

	// No AddExpectedError: every cycle drains cleanly.
	constexpr int32 Cycles = 200;
	const FRegistryRef Registry = MakeRegistry();
	const FName Name(TEXT("LifetimeTestCycles"));
	FSubscribedOwner Owner(Registry, Name);

	int32 Registered = 0;
	int32 Started = 0;
	int32 Released = 0;
	for (int32 Cycle = 0; Cycle < Cycles; ++Cycle)
	{
		FO3DTransportRegistration Registration = Registry->Register(MakeFakeDescriptor(Name));
		Registered += Registration.IsValid() ? 1 : 0;
		Started += Owner.Start() ? 1 : 0;
		Registration.Reset();
		Released += (!Owner.Sender.IsValid() && !Owner.Receiver.IsValid() && Registry->GetNumLiveInstances(Name) == 0) ? 1 : 0;
	}

	TestEqual(TEXT("Every cycle registered"), Registered, Cycles);
	TestEqual(TEXT("Every cycle started a session"), Started, Cycles);
	TestEqual(TEXT("Every cycle drained to zero live instances"), Released, Cycles);
	TestEqual(TEXT("One notification per cycle"), Owner.Notifications, Cycles);
	TestEqual(TEXT("Nothing registered at the end"), Registry->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportLifetimeCreateRaceTest, "Open3DBroadcast.Shared.TransportLifetime.InstanceCreatedDuringUnregisterIsDiscarded", O3DB_TEST_FLAGS)
bool FO3DTransportLifetimeCreateRaceTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportLifetimeTest;

	AddExpectedError(TEXT("unregistered while a sender was being created"), EAutomationExpectedMessageFlags::Contains, 1);

	// The factory unregisters its own transport before it returns, which is the order a module
	// shutting down while another thread creates an instance can produce. The instance it then
	// returns would never be drained, so the registry stops it and hands out nothing.
	const FRegistryRef Registry = MakeRegistry();
	const FName Name(TEXT("LifetimeTestCreateRace"));
	TSharedRef<FO3DTransportRegistration> Registration = MakeShared<FO3DTransportRegistration>();
	TWeakPtr<FO3DFakeSender, ESPMode::ThreadSafe> Created;

	FO3DTransportDescriptor Descriptor = MakeFakeDescriptor(Name);
	Descriptor.CreateSender = [Registration, &Created]() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>
	{
		TSharedRef<FO3DFakeSender, ESPMode::ThreadSafe> Sender = MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>();
		Created = Sender;
		Registration->Reset();
		return Sender;
	};
	*Registration = Registry->Register(MoveTemp(Descriptor));
	if (!TestTrue(TEXT("Registered"), Registration->IsValid()))
	{
		return false;
	}

	const TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> Result = Registry->CreateSender(Name);
	TestFalse(TEXT("Nothing is handed out once the transport unregistered"), Result.IsValid());
	TestFalse(TEXT("The discarded instance was released"), Created.IsValid());
	TestEqual(TEXT("Nothing live"), Registry->GetNumLiveInstances(Name), 0);
	TestFalse(TEXT("The transport is gone"), Registry->Find(Name).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportLifetimeSenderComponentTest, "Open3DBroadcast.Shared.TransportLifetime.SenderComponentReleasesOnUnregister", O3DB_TEST_FLAGS)
bool FO3DTransportLifetimeSenderComponentTest::RunTest(const FString& Parameters)
{
	// Expected by design: the component has no mesh (control-only), and the controller says why
	// it drops the sender. Any leak Error would fail the test.
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedError(TEXT("is being unregistered"), EAutomationExpectedMessageFlags::Contains, 1);

	TUniquePtr<FO3DFakeTransportScope> Scope = MakeUnique<FO3DFakeTransportScope>();
	const FName Name = Scope->GetName();

	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->bAutoCreateTransport = true;
	Component->bAllowControlOnly = true;
	Component->SetTransportName(Name);
	Component->StartCapture();

	const TWeakPtr<FO3DFakeSender> WeakSender = Scope->GetLastSender();
	if (!TestTrue(TEXT("The component started a sender of the fake transport"), WeakSender.IsValid()))
	{
		Component->StopCapture();
		return false;
	}
	TestEqual(TEXT("The registry counts the component's sender"), FO3DTransportRegistry::Get().GetNumLiveInstances(Name), 1);

	// The transport's module shuts down mid-session.
	Scope.Reset();

	TestFalse(TEXT("The component released its sender"), WeakSender.IsValid());
	TestEqual(TEXT("Nothing of the transport is live"), FO3DTransportRegistry::Get().GetNumLiveInstances(Name), 0);

	Component->StopCapture();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportLifetimeReceiverSourceTest, "Open3DBroadcast.Shared.TransportLifetime.ReceiverSourceReleasesOnUnregister", O3DB_TEST_FLAGS)
bool FO3DTransportLifetimeReceiverSourceTest::RunTest(const FString& Parameters)
{
	// Expected by design: the source says why it drops the receiver. Any leak Error would fail the test.
	AddExpectedError(TEXT("is being unregistered"), EAutomationExpectedMessageFlags::Contains, 1);

	TUniquePtr<FO3DFakeTransportScope> Scope = MakeUnique<FO3DFakeTransportScope>();
	const FName Name = Scope->GetName();

	FO3DReceiverSourceConfig Config;
	Config.TransportName = Name;
	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Config);
	if (!TestTrue(TEXT("The source started the fake transport"), FO3DReceiverSourceTestAccessor::StartTransport(*Source)))
	{
		return false;
	}

	const TWeakPtr<FO3DFakeReceiver> WeakReceiver = Scope->GetLastReceiver();
	{
		const TSharedPtr<FO3DFakeReceiver> Receiver = WeakReceiver.Pin();
		if (!TestTrue(TEXT("The source holds a receiver"), Receiver.IsValid()))
		{
			FO3DReceiverSourceTestAccessor::StopTransport(*Source);
			return false;
		}
		TestTrue(TEXT("The receiver has the source's consumer"), Receiver->HasConsumer());
		TestTrue(TEXT("The receiver has the source's control sink"), Receiver->HasControlSink());
	}
	TestEqual(TEXT("The registry counts the source's receiver"), FO3DTransportRegistry::Get().GetNumLiveInstances(Name), 1);

	// The transport's module shuts down mid-session.
	Scope.Reset();

	TestFalse(TEXT("The source released its receiver (and with it the sinks it gave it)"), WeakReceiver.IsValid());
	TestEqual(TEXT("Nothing of the transport is live"), FO3DTransportRegistry::Get().GetNumLiveInstances(Name), 0);

	// Stopping again is harmless.
	FO3DReceiverSourceTestAccessor::StopTransport(*Source);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
