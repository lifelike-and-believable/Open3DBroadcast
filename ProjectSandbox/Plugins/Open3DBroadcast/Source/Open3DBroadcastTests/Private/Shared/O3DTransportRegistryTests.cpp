// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 1 (ADR 0007 item 4; SHR-12, SND-23, RCV-27, RCV-28): the one transport registry.
// - Register and unregister through the move-only registration handle.
// - A duplicate name is refused and the first registration keeps it; a stale handle never
//   removes a later registration.
// - Find hands out a shared, immutable descriptor that stays usable after the transport
//   unregisters (no pointer into the registry's map, RCV-27).
// - Lookups on worker threads while the game thread registers and unregisters stay consistent.
// - The picker list for a role is exactly the set of names that role can instantiate (RCV-28).
// (WP-A1 step 6 removed the deprecated register functions and their test.)
//
// Most cases use a private registry (MakeShared), so they never touch the process-wide one the
// transports register with. No network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Templates/UniquePtr.h"

#include "O3DTestFakes.h"
#include "Transport/O3DTransportRegistry.h"

#include <atomic>

namespace O3DTransportRegistryTest
{
	using FRegistryRef = TSharedRef<FO3DTransportRegistry, ESPMode::ThreadSafe>;

	FRegistryRef MakeRegistry()
	{
		return MakeShared<FO3DTransportRegistry, ESPMode::ThreadSafe>();
	}

	FO3DSenderFactory MakeSenderFactory()
	{
		return []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender>(); };
	}

	FO3DReceiverFactory MakeReceiverFactory()
	{
		return []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeReceiver>(); };
	}

	/** A descriptor named Name with the factories asked for and one sender option, a secret. */
	FO3DTransportDescriptor MakeDescriptor(FName Name, bool bSender, bool bReceiver)
	{
		FO3DTransportDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
		if (bSender)
		{
			Descriptor.CreateSender = MakeSenderFactory();
		}
		if (bReceiver)
		{
			Descriptor.CreateReceiver = MakeReceiverFactory();
		}
		// A Secret entry declares the secret key.
		FO3DTransportOptionField Field;
		Field.Key = TEXT("o3dregistrytest.token");
		Field.DisplayName = FText::FromString(TEXT("Token"));
		Field.Type = EO3DTransportOptionType::Secret;
		Descriptor.SenderOptions.OptionSchema.Add(Field);
		return Descriptor;
	}

	/** Names a role can actually instantiate: the ones whose factory returns an instance. */
	TArray<FName> GetCreatableNames(const FO3DTransportRegistry& Registry, const TArray<FName>& Candidates, EO3DTransportRole Role)
	{
		TArray<FName> Creatable;
		for (const FName& Name : Candidates)
		{
			const bool bCreated = Role == EO3DTransportRole::Sender
				? Registry.CreateSender(Name).IsValid()
				: Registry.CreateReceiver(Name).IsValid();
			if (bCreated)
			{
				Creatable.Add(Name);
			}
		}
		Creatable.Sort(FNameLexicalLess());
		return Creatable;
	}

	/** Looks names up in a loop and records any inconsistency it sees. */
	class FLookupWorker final : public FRunnable
	{
	public:
		FLookupWorker(FRegistryRef InRegistry, TArray<FName> InNames)
			: Registry(MoveTemp(InRegistry))
			, Names(MoveTemp(InNames))
		{
		}

		virtual uint32 Run() override
		{
			while (!bStop.load())
			{
				for (const FName& Name : Names)
				{
					const FO3DTransportDescriptorPtr Descriptor = Registry->Find(Name);
					if (Descriptor.IsValid())
					{
						++Found;
						if (Descriptor->Name != Name || !Descriptor->CreateSender)
						{
							++Inconsistent;
						}
						// Calling the factory without the lock, on a pinned descriptor, is the point.
						else if (!Descriptor->CreateSender().IsValid())
						{
							++Inconsistent;
						}
					}
				}
				for (const FName& Listed : Registry->GetNames(EO3DTransportRole::Sender))
				{
					if (!Names.Contains(Listed))
					{
						++Inconsistent;
					}
				}
				++Iterations;
			}
			return 0;
		}

		virtual void Stop() override
		{
			bStop.store(true);
		}

		FRegistryRef Registry;
		const TArray<FName> Names;
		std::atomic<bool> bStop{false};
		std::atomic<int32> Iterations{0};
		std::atomic<int32> Found{0};
		std::atomic<int32> Inconsistent{0};
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportRegistryRegisterTest, "Open3DBroadcast.Shared.TransportRegistry.RegisterAndUnregister", O3DB_TEST_FLAGS)
bool FO3DTransportRegistryRegisterTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportRegistryTest;

	// The missing receiver factory is logged at Warning by design.
	AddExpectedError(TEXT("No receiver factory registered for transport 'RegistryTestA'"), EAutomationExpectedMessageFlags::Contains, 1);

	const FRegistryRef Registry = MakeRegistry();
	int32 Changes = 0;
	Registry->OnTransportsChanged().AddLambda([&Changes]() { ++Changes; });

	const FName Name(TEXT("RegistryTestA"));
	FO3DTransportRegistration Registration = Registry->Register(MakeDescriptor(Name, /*bSender=*/true, /*bReceiver=*/false));
	if (!TestTrue(TEXT("Registration is valid"), Registration.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("Registration names the transport"), Registration.GetName() == Name);
	TestEqual(TEXT("One change broadcast"), Changes, 1);
	TestEqual(TEXT("One entry"), Registry->Num(), 1);

	const FO3DTransportDescriptorPtr Found = Registry->Find(Name);
	TestTrue(TEXT("Find returns the descriptor"), Found.IsValid() && Found->Name == Name);
	TestTrue(TEXT("Registered for the sender role"), Registry->IsRegistered(Name, EO3DTransportRole::Sender));
	TestFalse(TEXT("Not registered for the receiver role"), Registry->IsRegistered(Name, EO3DTransportRole::Receiver));
	TestTrue(TEXT("Listed for senders"), Registry->GetNames(EO3DTransportRole::Sender).Contains(Name));
	TestFalse(TEXT("Not listed for receivers"), Registry->GetNames(EO3DTransportRole::Receiver).Contains(Name));
	TestTrue(TEXT("CreateSender makes an instance"), Registry->CreateSender(Name).IsValid());
	TestFalse(TEXT("CreateReceiver has no factory"), Registry->CreateReceiver(Name).IsValid());

	TArray<FString> SecretKeys;
	TMap<FString, FString> SecretEnvVars;
	TestTrue(TEXT("Secret declaration found"), Registry->GetSecretDeclaration(Name, EO3DTransportRole::Sender, SecretKeys, SecretEnvVars));
	TestTrue(TEXT("Secret key copied"), SecretKeys.Contains(TEXT("o3dregistrytest.token")));
	FO3DTransportOptionSchema Schema;
	TestTrue(TEXT("Schema found"), Registry->GetOptionSchema(Name, EO3DTransportRole::Sender, Schema));
	TestEqual(TEXT("Schema copied"), Schema.Num(), 1);

	// Moving the handle moves the ownership; the moved-from handle does nothing.
	FO3DTransportRegistration Moved = MoveTemp(Registration);
	TestFalse(TEXT("Moved-from handle is empty"), Registration.IsValid());
	Registration.Reset();
	TestTrue(TEXT("Resetting the moved-from handle unregisters nothing"), Registry->Find(Name).IsValid());

	Moved.Reset();
	TestFalse(TEXT("Handle is empty after Reset"), Moved.IsValid());
	TestFalse(TEXT("Find returns null after unregister"), Registry->Find(Name).IsValid());
	TestEqual(TEXT("No entries"), Registry->Num(), 0);
	TestEqual(TEXT("Unregister broadcast a change"), Changes, 2);
	Moved.Reset();
	TestEqual(TEXT("Reset is idempotent"), Changes, 2);

	// A handle that outlives its registry does nothing when it goes.
	{
		FRegistryRef ShortLived = MakeRegistry();
		FO3DTransportRegistration Orphan = ShortLived->Register(MakeDescriptor(Name, true, true));
		TestTrue(TEXT("Registered with the short-lived registry"), Orphan.IsValid());
		ShortLived = MakeRegistry();
		TestFalse(TEXT("Handle is invalid once its registry is gone"), Orphan.IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportRegistryRejectTest, "Open3DBroadcast.Shared.TransportRegistry.RejectsInvalidDescriptors", O3DB_TEST_FLAGS)
bool FO3DTransportRegistryRejectTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportRegistryTest;

	AddExpectedError(TEXT("the descriptor has no name"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedError(TEXT("neither a sender nor a receiver factory"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedError(TEXT("built for transport API version"), EAutomationExpectedMessageFlags::Contains, 1);

	const FRegistryRef Registry = MakeRegistry();

	FO3DTransportRegistration NoName = Registry->Register(MakeDescriptor(NAME_None, true, true));
	TestFalse(TEXT("A descriptor without a name is refused"), NoName.IsValid());

	FO3DTransportRegistration NoFactory = Registry->Register(MakeDescriptor(TEXT("RegistryTestNoFactory"), false, false));
	TestFalse(TEXT("A descriptor without factories is refused"), NoFactory.IsValid());

	FO3DTransportDescriptor WrongVersion = MakeDescriptor(TEXT("RegistryTestVersion"), true, true);
	WrongVersion.ApiVersion = O3D_TRANSPORT_API_VERSION + 1;
	FO3DTransportRegistration Mismatch = Registry->Register(MoveTemp(WrongVersion));
	TestFalse(TEXT("A descriptor built for another API version is refused"), Mismatch.IsValid());

	TestEqual(TEXT("Nothing was registered"), Registry->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportRegistryDuplicateTest, "Open3DBroadcast.Shared.TransportRegistry.DuplicateNameKeepsFirst", O3DB_TEST_FLAGS)
bool FO3DTransportRegistryDuplicateTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportRegistryTest;

	AddExpectedError(TEXT("a transport with that name is already registered"), EAutomationExpectedMessageFlags::Contains, 1);

	const FRegistryRef Registry = MakeRegistry();
	const FName Name(TEXT("RegistryTestDup"));

	FO3DTransportRegistration First = Registry->Register(MakeDescriptor(Name, /*bSender=*/true, /*bReceiver=*/false));
	const FO3DTransportDescriptorPtr FirstDescriptor = Registry->Find(Name);
	FO3DTransportRegistration Second = Registry->Register(MakeDescriptor(Name, /*bSender=*/true, /*bReceiver=*/true));

	TestTrue(TEXT("First registration is valid"), First.IsValid());
	TestFalse(TEXT("Second registration under the same name is refused"), Second.IsValid());
	TestTrue(TEXT("The first descriptor keeps the name"), Registry->Find(Name) == FirstDescriptor);
	TestFalse(TEXT("The refused descriptor's receiver factory is not visible"), Registry->IsRegistered(Name, EO3DTransportRole::Receiver));

	// Once the first registration goes, the name is free again, and the old handle cannot remove
	// the new registration.
	FO3DTransportRegistration Stale = MoveTemp(First);
	Stale.Reset();
	FO3DTransportRegistration Third = Registry->Register(MakeDescriptor(Name, true, true));
	TestTrue(TEXT("The name can be registered again"), Third.IsValid());
	Stale.Reset();
	First.Reset();
	Second.Reset();
	TestTrue(TEXT("Stale and refused handles leave the new registration alone"), Registry->IsRegistered(Name, EO3DTransportRole::Receiver));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportRegistryCopyTest, "Open3DBroadcast.Shared.TransportRegistry.LookupSurvivesUnregister", O3DB_TEST_FLAGS)
bool FO3DTransportRegistryCopyTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportRegistryTest;

	const FRegistryRef Registry = MakeRegistry();
	const FName Name(TEXT("RegistryTestCopy"));

	FO3DTransportDescriptor Descriptor = MakeDescriptor(Name, true, true);
	Descriptor.ConfigureReceiver = [](const FO3DTransportOptionsView&, FO3DTransportConfig& Config)
	{
		Config.StreamId = TEXT("configured");
	};
	FO3DTransportRegistration Registration = Registry->Register(MoveTemp(Descriptor));

	const FO3DTransportDescriptorPtr Held = Registry->Find(Name);
	FO3DTransportOptionSchema Schema;
	Registry->GetOptionSchema(Name, EO3DTransportRole::Sender, Schema);
	if (!TestTrue(TEXT("Descriptor found"), Held.IsValid()))
	{
		return false;
	}

	Registration.Reset();
	TestFalse(TEXT("The registry no longer has the name"), Registry->Find(Name).IsValid());
	TestFalse(TEXT("The registry no longer lists it"), Registry->GetNames(EO3DTransportRole::Sender).Contains(Name));

	// RCV-27: what the caller holds is its own reference, not a pointer into the map.
	TestTrue(TEXT("The held descriptor keeps its name"), Held->Name == Name);
	TestTrue(TEXT("The held sender factory still works"), Held->CreateSender && Held->CreateSender().IsValid());
	TestTrue(TEXT("The held receiver factory still works"), Held->CreateReceiver && Held->CreateReceiver().IsValid());
	TestTrue(TEXT("The held configure function is still set"), static_cast<bool>(Held->ConfigureReceiver));
	TestEqual(TEXT("The copied schema is intact"), Schema.Num(), 1);
	TestEqual(TEXT("The held descriptor's schema is intact"), Held->SenderOptions.OptionSchema.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportRegistryConcurrencyTest, "Open3DBroadcast.Shared.TransportRegistry.ConcurrentRegisterAndLookup", O3DB_TEST_FLAGS)
bool FO3DTransportRegistryConcurrencyTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportRegistryTest;

	constexpr int32 NumWorkers = 4;
	constexpr int32 NumNames = 8;
	constexpr int32 Rounds = 200;

	const FRegistryRef Registry = MakeRegistry();
	TArray<FName> Names;
	for (int32 Index = 0; Index < NumNames; ++Index)
	{
		Names.Add(FName(*FString::Printf(TEXT("RegistryTestConcurrent%d"), Index)));
	}

	TArray<TUniquePtr<FLookupWorker>> Workers;
	TArray<FRunnableThread*> Threads;
	for (int32 Index = 0; Index < NumWorkers; ++Index)
	{
		Workers.Add(MakeUnique<FLookupWorker>(Registry, Names));
		FRunnableThread* Thread = FRunnableThread::Create(Workers.Last().Get(), *FString::Printf(TEXT("O3DRegistryLookup_%d"), Index));
		if (Thread == nullptr)
		{
			AddError(TEXT("Could not start a lookup thread"));
			for (FRunnableThread* Started : Threads)
			{
				Started->Kill(/*bShouldWait=*/true);
				delete Started;
			}
			return false;
		}
		Threads.Add(Thread);
	}

	// Registration is a game-thread operation by contract; lookups run on the workers meanwhile.
	int32 Registered = 0;
	for (int32 Round = 0; Round < Rounds; ++Round)
	{
		TArray<FO3DTransportRegistration> Registrations;
		for (const FName& Name : Names)
		{
			Registrations.Add(Registry->Register(MakeDescriptor(Name, true, Round % 2 == 0)));
			Registered += Registrations.Last().IsValid() ? 1 : 0;
		}
		// Every name registered this round is visible until its handle goes.
		TestEqual(TEXT("All names listed while registered"), Registry->GetNames(EO3DTransportRole::Sender).Num(), NumNames);
		Registrations.Reset();
	}

	// Let the workers see at least a few full passes, then stop them.
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	bool bAllIterated = false;
	while (!bAllIterated && FPlatformTime::Seconds() < Deadline)
	{
		bAllIterated = true;
		for (const TUniquePtr<FLookupWorker>& Worker : Workers)
		{
			bAllIterated = bAllIterated && Worker->Iterations.load() >= 3;
		}
		if (!bAllIterated)
		{
			FPlatformProcess::Sleep(0.001f);
		}
	}

	for (int32 Index = 0; Index < Threads.Num(); ++Index)
	{
		Workers[Index]->Stop();
		Threads[Index]->WaitForCompletion();
		delete Threads[Index];
	}

	int32 Inconsistent = 0;
	for (const TUniquePtr<FLookupWorker>& Worker : Workers)
	{
		Inconsistent += Worker->Inconsistent.load();
		TestTrue(TEXT("Each worker ran"), Worker->Iterations.load() > 0);
	}
	TestEqual(TEXT("Every registration succeeded"), Registered, NumNames * Rounds);
	TestEqual(TEXT("No lookup saw an inconsistent descriptor or an unknown name"), Inconsistent, 0);
	TestEqual(TEXT("Everything unregistered at the end"), Registry->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportRegistryPickerTest, "Open3DBroadcast.Shared.TransportRegistry.PickersListCreatableSet", O3DB_TEST_FLAGS)
bool FO3DTransportRegistryPickerTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportRegistryTest;

	const FRegistryRef Registry = MakeRegistry();
	const FName SenderOnly(TEXT("RegistryTestSenderOnly"));
	const FName ReceiverOnly(TEXT("RegistryTestReceiverOnly"));
	const FName Both(TEXT("RegistryTestBoth"));

	FO3DTransportRegistration A = Registry->Register(MakeDescriptor(SenderOnly, true, false));
	FO3DTransportRegistration B = Registry->Register(MakeDescriptor(ReceiverOnly, false, true));
	FO3DTransportRegistration C = Registry->Register(MakeDescriptor(Both, true, true));
	// Every entry has a factory: Register refuses a descriptor without one
	// (RejectsInvalidDescriptors), so an options-only entry (RCV-28) cannot exist.

	const TArray<FName> AllNames = { SenderOnly, ReceiverOnly, Both };
	// CreateSender/CreateReceiver log a Warning for each name without that factory.
	AddExpectedError(TEXT("No sender factory registered for transport"), EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedError(TEXT("No receiver factory registered for transport"), EAutomationExpectedMessageFlags::Contains, 1);

	for (const EO3DTransportRole Role : { EO3DTransportRole::Sender, EO3DTransportRole::Receiver })
	{
		const TCHAR* RoleName = Role == EO3DTransportRole::Sender ? TEXT("sender") : TEXT("receiver");
		const TArray<FName> Listed = Registry->GetNames(Role);
		const TArray<FName> Creatable = GetCreatableNames(*Registry, AllNames, Role);
		TestTrue(*FString::Printf(TEXT("%s picker list equals the creatable set"), RoleName), Listed == Creatable);
	}
	TestTrue(TEXT("Sender picker"), Registry->GetNames(EO3DTransportRole::Sender) == TArray<FName>{ Both, SenderOnly });
	TestTrue(TEXT("Receiver picker"), Registry->GetNames(EO3DTransportRole::Receiver) == TArray<FName>{ Both, ReceiverOnly });

	// The process-wide registry obeys the same rule for every transport this build registered
	// (checked without creating instances of real transports).
	for (const EO3DTransportRole Role : { EO3DTransportRole::Sender, EO3DTransportRole::Receiver })
	{
		for (const FName& Name : FO3DTransportRegistry::Get().GetNames(Role))
		{
			const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(Name);
			TestTrue(*FString::Printf(TEXT("%s: listed transport has a factory for the role"), *Name.ToString()), Descriptor.IsValid() && Descriptor->HasRole(Role));
		}
	}
	TestTrue(TEXT("Loopback is listed for senders"), FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender).Contains(FName(TEXT("Loopback"))));
	TestTrue(TEXT("Loopback is listed for receivers"), FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver).Contains(FName(TEXT("Loopback"))));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
