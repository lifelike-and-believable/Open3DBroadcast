// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U3 (RCV-15, RCV-16, RCV-17): what a receiver source tells the user, and settings that apply live.
// - Status: a start that fails says why ("Error: ...") and the source reports itself invalid to
//   LiveLink; a started source waits for data, shows "Receiving via X" on the first frame, "No data
//   received" when frames stop, "Receiving" again when they resume, and the transport's
//   Reconnecting and Failed states (with the reason).
// - Create Source validation: an unregistered transport or options the transport refuses explain
//   why Create is disabled; a valid source validates clean.
// - Concealment: changing a concealment setting or turning concealment off drops the engines, so
//   the next real frame creates them from the new values.
// Sources are added to the editor's LiveLink client (as LiveLink does) and removed at the end;
// test-only fake transports, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include "O3DReceiverSource.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DTestFakes.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DReceiverStatusTest
{
	/** A receiver-only fake transport under a unique name; keeps the instance it creates. */
	struct FScopedFakeReceiverTransport
	{
		FName Name;
		FString CheckedKey;
		TSharedRef<TSharedPtr<FO3DFakeReceiver, ESPMode::ThreadSafe>> Created = MakeShared<TSharedPtr<FO3DFakeReceiver, ESPMode::ThreadSafe>>();
		FO3DTransportRegistration Registration;

		FScopedFakeReceiverTransport()
			: Name(*O3DTests::MakeUniqueName(TEXT("O3DStatusReceiver")))
		{
			// One option the transport refuses when it is "bad", for the validation test.
			CheckedKey = Name.ToString().ToLower() + TEXT(".checked");
			FO3DTransportOptionField Checked;
			Checked.Key = CheckedKey;
			Checked.Type = EO3DTransportOptionType::String;
			Checked.Validate = [](const FString& Value, FText& OutError)
			{
				if (Value == TEXT("bad"))
				{
					OutError = FText::FromString(TEXT("checked must not be bad"));
					return false;
				}
				return true;
			};

			const TSharedRef<TSharedPtr<FO3DFakeReceiver, ESPMode::ThreadSafe>> Holder = Created;
			FO3DTransportDescriptor Descriptor;
			Descriptor.Name = Name;
			Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
			Descriptor.ReceiverOptions.OptionSchema.Add(Checked);
			Descriptor.CreateReceiver = [Holder]() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>
			{
				*Holder = MakeShared<FO3DFakeReceiver, ESPMode::ThreadSafe>();
				return *Holder;
			};
			Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
		}

		~FScopedFakeReceiverTransport()
		{
			// The test's own reference goes before the transport unregisters (ADR 0007 item 5).
			Created->Reset();
			Registration.Reset();
		}

		FScopedFakeReceiverTransport(const FScopedFakeReceiverTransport&) = delete;
		FScopedFakeReceiverTransport& operator=(const FScopedFakeReceiverTransport&) = delete;
	};

	ILiveLinkClient* GetClient()
	{
		IModularFeatures& Features = IModularFeatures::Get();
		return Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName)
			? &Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName)
			: nullptr;
	}

	/** Adds Source to LiveLink (which starts it) and removes it on exit. */
	struct FScopedLiveLinkSource
	{
		ILiveLinkClient& Client;
		TSharedRef<FO3DReceiverSource> Source;
		FGuid Guid;

		FScopedLiveLinkSource(ILiveLinkClient& InClient, const FO3DReceiverSourceConfig& Config)
			: Client(InClient)
			, Source(MakeShared<FO3DReceiverSource>(Config))
		{
			Guid = Client.AddSource(Source);
		}

		~FScopedLiveLinkSource()
		{
			Source->RequestSourceShutdown();
			if (Guid.IsValid())
			{
				Client.RemoveSource(Guid);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverStatusStartFailureTest, "Open3DBroadcast.Receiver.Status.StartFailureSaysWhy", O3DB_TEST_FLAGS)
bool FO3DReceiverStatusStartFailureTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverStatusTest;
	ILiveLinkClient* Client = GetClient();
	if (!TestNotNull(TEXT("LiveLink client"), Client))
	{
		return false;
	}

	FO3DReceiverSourceConfig Config;
	Config.TransportName = FName(*O3DTests::MakeUniqueName(TEXT("O3DNoSuchReceiver")));
	AddExpectedError(TEXT("No receiver registered for transport"), EAutomationExpectedMessageFlags::Contains, 1);
	FScopedLiveLinkSource Added(*Client, Config);

	const FString Status = FO3DReceiverSourceTestAccessor::GetSourceStatus(*Added.Source).ToString();
	TestTrue(TEXT("The status says why: ") + Status, Status.StartsWith(TEXT("Error:")) && Status.Contains(TEXT("No receiver is registered")));
	TestTrue(TEXT("The start is recorded as failed"), FO3DReceiverSourceTestAccessor::HasStartFailed(*Added.Source));
	TestFalse(TEXT("LiveLink sees the source as not valid"), Added.Source->IsSourceStillValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverStatusLifecycleTest, "Open3DBroadcast.Receiver.Status.FollowsDataAndConnection", O3DB_TEST_FLAGS)
bool FO3DReceiverStatusLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverStatusTest;
	ILiveLinkClient* Client = GetClient();
	if (!TestNotNull(TEXT("LiveLink client"), Client))
	{
		return false;
	}
	FScopedFakeReceiverTransport Transport;

	FO3DReceiverSourceConfig Config;
	Config.TransportName = Transport.Name;
	{
		FScopedLiveLinkSource Added(*Client, Config);
		FO3DReceiverSource& Source = *Added.Source;
		const auto Status = [&Source]() { return FO3DReceiverSourceTestAccessor::GetSourceStatus(Source).ToString(); };

		if (!TestTrue(TEXT("Started: ") + Status(), Transport.Created->IsValid()))
		{
			return false;
		}
		TestTrue(TEXT("Waiting for data: ") + Status(), Status().StartsWith(TEXT("Waiting for data via")));
		TestTrue(TEXT("LiveLink sees the source as valid"), Source.IsSourceStillValid());

		// Any bytes count as data arriving (they are rejected later as malformed, which only logs).
		(*Transport.Created)->Enqueue({ 1, 2, 3 });
		FO3DReceiverSourceTestAccessor::Poll(Source);
		TestTrue(TEXT("First frame: ") + Status(), Status().StartsWith(TEXT("Receiving via")));

		FO3DReceiverSourceTestAccessor::UpdateStalledStatus(Source, FPlatformTime::Seconds() + 5.0);
		TestTrue(TEXT("Frames stopped: ") + Status(), Status().StartsWith(TEXT("No data received")));

		(*Transport.Created)->Enqueue({ 4, 5, 6 });
		FO3DReceiverSourceTestAccessor::Poll(Source);
		TestTrue(TEXT("Frames resumed: ") + Status(), Status().StartsWith(TEXT("Receiving via")));

		(*Transport.Created)->SimulateConnectionState(EO3DConnectionState::Reconnecting);
		FO3DReceiverSourceTestAccessor::DrainConnectionState(Source);
		TestTrue(TEXT("Reconnecting: ") + Status(), Status().StartsWith(TEXT("Reconnecting via")));

		(*Transport.Created)->SimulateConnectionState(EO3DConnectionState::Failed, FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, TEXT("relay refused")));
		FO3DReceiverSourceTestAccessor::DrainConnectionState(Source);
		TestTrue(TEXT("Failed, with the reason: ") + Status(), Status().StartsWith(TEXT("Error:")) && Status().Contains(TEXT("relay refused")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverValidateNewSourceTest, "Open3DBroadcast.Receiver.SourceFactory.ValidateNewSource", O3DB_TEST_FLAGS)
bool FO3DReceiverValidateNewSourceTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverStatusTest;
	FScopedFakeReceiverTransport Transport;

	FO3DReceiverSourceConfig Config;
	Config.TransportName = FName(*O3DTests::MakeUniqueName(TEXT("O3DNoSuchReceiver")));
	TestTrue(TEXT("Unregistered transport"), O3DReceiver::ValidateNewSource(Config).ToString().Contains(TEXT("No receiver is registered")));

	Config.TransportName = Transport.Name;
	TestTrue(TEXT("A registered transport with valid options"), O3DReceiver::ValidateNewSource(Config).IsEmpty());

	Config.TransportOptions.Add(Transport.CheckedKey, TEXT("bad"));
	TestTrue(TEXT("Options the transport refuses"), O3DReceiver::ValidateNewSource(Config).ToString().Contains(TEXT("checked must not be bad")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverConcealmentSettingsLiveTest, "Open3DBroadcast.Receiver.Concealment.SettingsEditsApply", O3DB_TEST_FLAGS)
bool FO3DReceiverConcealmentSettingsLiveTest::RunTest(const FString& Parameters)
{
	UO3DReceiverSourceSettings* Settings = NewObject<UO3DReceiverSourceSettings>(GetTransientPackage());
	Settings->bEnableConcealment = true;
	FO3DReceiverConcealmentProbe Probe;
	const TArray<FTransform> Pose = { FTransform::Identity };

	Probe.ObserveRealFrame(Settings, TEXT("Hero"), 1.0, Pose, false);
	TestEqual(TEXT("An engine for the subject"), Probe.GetNumEngines(), 1);

	Probe.Tick(Settings, false, 1.01);
	TestEqual(TEXT("Unchanged settings keep the engine"), Probe.GetNumEngines(), 1);

	Settings->StarvationThresholdMs += 25.0f;
	Probe.Tick(Settings, false, 1.02);
	TestEqual(TEXT("A changed setting drops the engine"), Probe.GetNumEngines(), 0);

	Probe.ObserveRealFrame(Settings, TEXT("Hero"), 1.03, Pose, false);
	TestEqual(TEXT("The next real frame creates it from the new values"), Probe.GetNumEngines(), 1);

	Settings->bEnableConcealment = false;
	Probe.Tick(Settings, false, 1.04);
	TestEqual(TEXT("Turning concealment off frees the engines"), Probe.GetNumEngines(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
