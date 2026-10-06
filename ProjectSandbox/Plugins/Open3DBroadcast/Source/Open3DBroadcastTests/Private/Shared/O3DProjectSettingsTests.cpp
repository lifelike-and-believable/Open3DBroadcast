// Copyright 2026 Lifelike & Believable. All Rights Reserved.

// WP-U1 (UX-2, RCV-18): project-wide transport defaults (UOpen3DBroadcastSettings).
// - An option a sender component or receiver source leaves unset (absent or whitespace) takes the
//   project default for its transport and side; its own value wins. The default reaches the
//   transport's configure function and Config.AdvancedParams, which Sockets, NNG and Loopback read.
// - A default for an option the transport declares Secret is never applied (ADR 0004), and the
//   defaults of an unregistered transport are not applied.
// - Options panels show the project default as a field's default.
// - A receiver source created without a connection string starts with no options of its own,
//   whatever the old GameUserSettings.ini defaults hold (RCV-18).
// Test-only transport names and keys under the process-wide registry. The settings objects are
// changed in memory and restored; nothing is saved to an ini.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include "O3DReceiverSource.h"
#include "O3DReceiverSourceFactory.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "Open3DBroadcastSettings.h"
#include "Testing/O3DReceiverTesting.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DProjectSettingsTest
{
	/** What a configure function saw. */
	struct FSeen
	{
		int32 Calls = 0;
		TMap<FString, FString> Values;
	};

	/**
	 * Registers a sender and receiver descriptor for a unique name, with the same schema on both
	 * sides: url (Url), host (String, default 127.0.0.1), port (Int, default 17000) and token
	 * (Secret). Unregisters it on exit.
	 */
	struct FScopedDefaultsTransport
	{
		FName Name;
		FString UrlKey;
		FString HostKey;
		FString PortKey;
		FString SecretKey;
		TSharedRef<FSeen> SenderSeen = MakeShared<FSeen>();
		TSharedRef<FSeen> ReceiverSeen = MakeShared<FSeen>();
		FO3DTransportOptionSchema Schema;
		FO3DTransportRegistration Registration;

		FScopedDefaultsTransport()
			: Name(*O3DTests::MakeUniqueName(TEXT("O3DProjectDefaults")))
		{
			const FString Prefix = Name.ToString().ToLower();
			UrlKey = Prefix + TEXT(".url");
			HostKey = Prefix + TEXT(".host");
			PortKey = Prefix + TEXT(".port");
			SecretKey = Prefix + TEXT(".token");

			FO3DTransportOptionField Url;
			Url.Key = UrlKey;
			Url.Type = EO3DTransportOptionType::Url;
			Schema.Add(Url);
			FO3DTransportOptionField Host;
			Host.Key = HostKey;
			Host.Type = EO3DTransportOptionType::String;
			Host.Default = TEXT("127.0.0.1");
			Schema.Add(Host);
			FO3DTransportOptionField Port;
			Port.Key = PortKey;
			Port.Type = EO3DTransportOptionType::Int;
			Port.Default = TEXT("17000");
			Schema.Add(Port);
			FO3DTransportOptionField Secret;
			Secret.Key = SecretKey;
			Secret.Type = EO3DTransportOptionType::Secret;
			Schema.Add(Secret);

			const TSharedRef<FSeen> SenderRecord = SenderSeen;
			const TSharedRef<FSeen> ReceiverRecord = ReceiverSeen;
			FO3DTransportDescriptor Descriptor;
			Descriptor.Name = Name;
			Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
			Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>(); };
			Descriptor.CreateReceiver = []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeReceiver, ESPMode::ThreadSafe>(); };
			Descriptor.SenderOptions.OptionSchema = Schema;
			Descriptor.ReceiverOptions.OptionSchema = Schema;
			Descriptor.ConfigureSender = [SenderRecord](const FO3DTransportOptionsView& Options, FO3DTransportConfig& Config)
			{
				++SenderRecord->Calls;
				SenderRecord->Values = Options.GetValues();
			};
			Descriptor.ConfigureReceiver = [ReceiverRecord](const FO3DTransportOptionsView& Options, FO3DTransportConfig& Config)
			{
				++ReceiverRecord->Calls;
				ReceiverRecord->Values = Options.GetValues();
			};
			Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
		}

		~FScopedDefaultsTransport()
		{
			FO3DSecretStore::Get().Clear(Name.ToString(), TEXT("default"), SecretKey);
			Registration.Reset();
		}

		FScopedDefaultsTransport(const FScopedDefaultsTransport&) = delete;
		FScopedDefaultsTransport& operator=(const FScopedDefaultsTransport&) = delete;
	};

	/** Restores the project settings object on exit. Never saves it. */
	struct FScopedProjectDefaults
	{
		TMap<FName, FO3DTransportDefaultOptions> SavedSender;
		TMap<FName, FO3DTransportDefaultOptions> SavedReceiver;

		FScopedProjectDefaults()
		{
			const UOpen3DBroadcastSettings* Settings = GetDefault<UOpen3DBroadcastSettings>();
			SavedSender = Settings->SenderDefaults;
			SavedReceiver = Settings->ReceiverDefaults;
		}

		~FScopedProjectDefaults()
		{
			UOpen3DBroadcastSettings* Settings = GetMutableDefault<UOpen3DBroadcastSettings>();
			Settings->SenderDefaults = SavedSender;
			Settings->ReceiverDefaults = SavedReceiver;
		}

		void Set(TMap<FName, FO3DTransportDefaultOptions>& Map, FName Transport, const TMap<FString, FString>& Options)
		{
			FO3DTransportDefaultOptions Defaults;
			Defaults.Options = Options;
			Map.Add(Transport, Defaults);
		}

		void SetSender(FName Transport, const TMap<FString, FString>& Options) { Set(GetMutableDefault<UOpen3DBroadcastSettings>()->SenderDefaults, Transport, Options); }
		void SetReceiver(FName Transport, const TMap<FString, FString>& Options) { Set(GetMutableDefault<UOpen3DBroadcastSettings>()->ReceiverDefaults, Transport, Options); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DProjectSettingsSenderDefaultsTest, "Open3DBroadcast.Shared.ProjectSettings.SenderDefaultsFillUnsetOptions", O3DB_TEST_FLAGS)
bool FO3DProjectSettingsSenderDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace O3DProjectSettingsTest;
	FScopedDefaultsTransport Transport;
	FScopedProjectDefaults Defaults;
	Defaults.SetSender(Transport.Name, {
		{ Transport.HostKey, TEXT("10.0.0.5") },
		{ Transport.PortKey, TEXT("9000") },
		{ Transport.UrlKey, TEXT("wss://project.invalid") },
		{ Transport.SecretKey, TEXT("PROJECT-SECRET-3b7d") } });
	Defaults.SetReceiver(Transport.Name, { { Transport.HostKey, TEXT("10.9.9.9") } });

	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->SetTransportName(Transport.Name);
	Component->TransportOptions.Add(Transport.PortKey, TEXT("9100"));
	Component->TransportOptions.Add(Transport.UrlKey, TEXT("   "));

	const FO3DTransportConfig Config = FO3DSenderComponentTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("The configure function ran once"), Transport.SenderSeen->Calls, 1);
	TestEqual(TEXT("Unset host: the project's sender default"), Transport.SenderSeen->Values.FindRef(Transport.HostKey), FString(TEXT("10.0.0.5")));
	TestEqual(TEXT("The component's own port wins"), Transport.SenderSeen->Values.FindRef(Transport.PortKey), FString(TEXT("9100")));
	TestEqual(TEXT("A whitespace url counts as unset"), Transport.SenderSeen->Values.FindRef(Transport.UrlKey), FString(TEXT("wss://project.invalid")));
	TestFalse(TEXT("A Secret option's project default is never applied"), Transport.SenderSeen->Values.Contains(Transport.SecretKey));
	TestEqual(TEXT("The default reaches AdvancedParams"), Config.AdvancedParams.FindRef(Transport.HostKey), FString(TEXT("10.0.0.5")));
	TestFalse(TEXT("No secret in AdvancedParams"), Config.AdvancedParams.Contains(Transport.SecretKey));
	TestFalse(TEXT("The project value is not used as the secret"), Config.Secrets.FindRef(Transport.SecretKey) == TEXT("PROJECT-SECRET-3b7d"));
	TestFalse(TEXT("The component's own options are unchanged"), Component->TransportOptions.Contains(Transport.HostKey));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DProjectSettingsReceiverDefaultsTest, "Open3DBroadcast.Shared.ProjectSettings.ReceiverDefaultsFillUnsetOptions", O3DB_TEST_FLAGS)
bool FO3DProjectSettingsReceiverDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace O3DProjectSettingsTest;
	FScopedDefaultsTransport Transport;
	FScopedProjectDefaults Defaults;
	Defaults.SetReceiver(Transport.Name, {
		{ Transport.HostKey, TEXT("0.0.0.0") },
		{ Transport.UrlKey, TEXT("wss://receiver.invalid") },
		{ Transport.SecretKey, TEXT("PROJECT-SECRET-77aa") } });
	Defaults.SetSender(Transport.Name, { { Transport.PortKey, TEXT("9000") } });

	FO3DReceiverSourceConfig SourceConfig;
	SourceConfig.TransportName = Transport.Name;
	SourceConfig.TransportOptions.Add(Transport.UrlKey, TEXT("wss://own.invalid"));
	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(SourceConfig);

	const FO3DTransportConfig Config = FO3DReceiverSourceTestAccessor::BuildTransportConfig(*Source);
	TestEqual(TEXT("The configure function ran once"), Transport.ReceiverSeen->Calls, 1);
	TestEqual(TEXT("Unset host: the project's receiver default"), Transport.ReceiverSeen->Values.FindRef(Transport.HostKey), FString(TEXT("0.0.0.0")));
	TestEqual(TEXT("The source's own url wins"), Transport.ReceiverSeen->Values.FindRef(Transport.UrlKey), FString(TEXT("wss://own.invalid")));
	TestFalse(TEXT("A sender default does not reach a receiver"), Transport.ReceiverSeen->Values.Contains(Transport.PortKey));
	TestFalse(TEXT("A Secret option's project default is never applied"), Transport.ReceiverSeen->Values.Contains(Transport.SecretKey));
	TestEqual(TEXT("The default reaches AdvancedParams"), Config.AdvancedParams.FindRef(Transport.HostKey), FString(TEXT("0.0.0.0")));
	TestFalse(TEXT("No secret in AdvancedParams"), Config.AdvancedParams.Contains(Transport.SecretKey));
	TestFalse(TEXT("The source's own options are unchanged"), Source->GetSourceSettings().TransportOptions.Contains(Transport.HostKey));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DProjectSettingsUnregisteredTest, "Open3DBroadcast.Shared.ProjectSettings.UnregisteredTransportIgnored", O3DB_TEST_FLAGS)
bool FO3DProjectSettingsUnregisteredTest::RunTest(const FString& Parameters)
{
	using namespace O3DProjectSettingsTest;
	FScopedProjectDefaults Defaults;
	const FName Unregistered(*O3DTests::MakeUniqueName(TEXT("O3DUnregisteredDefaults")));
	Defaults.SetSender(Unregistered, { { TEXT("host"), TEXT("10.0.0.5") } });

	TMap<FString, FString> Options;
	UOpen3DBroadcastSettings::ApplyTransportDefaults(Unregistered, EO3DTransportRole::Sender, Options);
	TestEqual(TEXT("Nothing applied for a transport whose secret options are unknown"), Options.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DProjectSettingsSchemaDefaultsTest, "Open3DBroadcast.Shared.ProjectSettings.PanelShowsProjectDefault", O3DB_TEST_FLAGS)
bool FO3DProjectSettingsSchemaDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace O3DProjectSettingsTest;
	FScopedDefaultsTransport Transport;
	FScopedProjectDefaults Defaults;
	Defaults.SetReceiver(Transport.Name, {
		{ Transport.HostKey, TEXT("0.0.0.0") },
		{ Transport.SecretKey, TEXT("PROJECT-SECRET-91fe") } });

	FO3DTransportOptionSchema Schema = Transport.Schema;
	UOpen3DBroadcastSettings::ApplyToSchemaDefaults(Transport.Name, EO3DTransportRole::Receiver, Schema);
	const auto FindField = [&Schema](const FString& Key) -> const FO3DTransportOptionField*
	{
		return Schema.FindByPredicate([&Key](const FO3DTransportOptionField& Field) { return Field.Key == Key; });
	};
	TestEqual(TEXT("Host shows the project default"), FindField(Transport.HostKey)->Default, FString(TEXT("0.0.0.0")));
	TestEqual(TEXT("Port keeps the transport's default"), FindField(Transport.PortKey)->Default, FString(TEXT("17000")));
	TestTrue(TEXT("A Secret field never shows a project value"), FindField(Transport.SecretKey)->Default.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverFactoryStartsWithoutOptionsTest, "Open3DBroadcast.Receiver.SourceFactory.NewSourceStartsWithoutOptions", O3DB_TEST_FLAGS)
bool FO3DReceiverFactoryStartsWithoutOptionsTest::RunTest(const FString& Parameters)
{
	// What a GameUserSettings.ini written before WP-U1 can hold: the options of the last source
	// created (RCV-18). They are not options of a new source; the project defaults are.
	UO3DReceiverSettingsObject* Legacy = GetMutableDefault<UO3DReceiverSettingsObject>();
	const FO3DReceiverSourceConfig Saved = Legacy->Settings;
	Legacy->Settings.TransportOptions.Add(TEXT("host"), TEXT("192.0.2.44"));

	const TSharedPtr<ILiveLinkSource> Created = GetDefault<UO3DReceiverSourceFactory>()->CreateSource(FString());
	const TSharedPtr<FO3DReceiverSource> Source = StaticCastSharedPtr<FO3DReceiverSource>(Created);
	if (TestTrue(TEXT("A source was created"), Source.IsValid()))
	{
		TestEqual(TEXT("It has no options of its own"), Source->GetSourceSettings().TransportOptions.Num(), 0);
	}
	TestEqual(TEXT("Creating a source leaves the legacy defaults alone"), Legacy->Settings.TransportOptions.FindRef(TEXT("host")), FString(TEXT("192.0.2.44")));

	Legacy->Settings = Saved;
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
