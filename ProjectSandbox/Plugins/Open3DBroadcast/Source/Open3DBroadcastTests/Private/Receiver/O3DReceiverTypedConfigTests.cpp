// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 5a (ADR 0007 items 4 and 8; SHR-36, SND-35): the receiver source and typed config.
// - The options saved in the source settings (project settings, LiveLink presets) reach the
//   transport's configure function through an FO3DTransportOptionsView with the schema; secrets
//   never do (ADR 0004).
// - O3DReceiver::SwitchTransport keeps the other transport's options (SND-35), never puts a
//   secret away, and the connection string carries only the selected transport's options.
// Test-only transport names and keys under the process-wide registry; no transport module, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "O3DReceiverSource.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DSecretStore.h"
#include "O3DTestFakes.h"
#include "Testing/O3DReceiverTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DReceiverTypedConfigTest
{
	struct FSeen
	{
		int32 Calls = 0;
		TMap<FString, FString> Values;
		bool bHadSchema = false;
	};

	/**
	 * Registers a descriptor for a unique name: a fake receiver factory, a schema with a Secret
	 * entry, and ConfigureReceiver. Unregisters it and clears the secret on exit.
	 */
	struct FScopedTypedReceiverTransport
	{
		FName Name;
		FString UrlKey;
		FString SecretKey;
		TSharedRef<FSeen> Seen = MakeShared<FSeen>();
		FO3DTransportRegistration Registration;

		FScopedTypedReceiverTransport()
			: Name(*O3DTests::MakeUniqueName(TEXT("O3DTypedReceiver")))
		{
			const FString Prefix = Name.ToString().ToLower();
			UrlKey = Prefix + TEXT(".url");
			SecretKey = Prefix + TEXT(".token");

			FO3DTransportOptionSchema Schema;
			FO3DTransportOptionField Url;
			Url.Key = UrlKey;
			Url.Type = EO3DTransportOptionType::Url;
			Url.Default = TEXT("wss://default.invalid");
			Schema.Add(Url);
			FO3DTransportOptionField Secret;
			Secret.Key = SecretKey;
			Secret.Type = EO3DTransportOptionType::Secret;
			Schema.Add(Secret);

			const TSharedRef<FSeen> Record = Seen;
			const FString UrlOption = UrlKey;
			FO3DTransportDescriptor Descriptor;
			Descriptor.Name = Name;
			Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
			Descriptor.CreateReceiver = []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeReceiver, ESPMode::ThreadSafe>(); };
			Descriptor.ReceiverOptions.OptionSchema = Schema;
			Descriptor.ConfigureReceiver = [Record, UrlOption](const FO3DTransportOptionsView& Options, FO3DTransportConfig& Config)
			{
				++Record->Calls;
				Record->Values = Options.GetValues();
				Record->bHadSchema = Options.GetSchema() != nullptr;
				Config.Uri = Options.GetString(UrlOption);
			};
			Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
		}

		~FScopedTypedReceiverTransport()
		{
			FO3DSecretStore::Get().Clear(Name.ToString(), TEXT("default"), SecretKey);
			Registration.Reset();
		}

		FScopedTypedReceiverTransport(const FScopedTypedReceiverTransport&) = delete;
		FScopedTypedReceiverTransport& operator=(const FScopedTypedReceiverTransport&) = delete;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverTypedConfigSavedOptionsTest, "Open3DBroadcast.Receiver.TypedConfig.SavedOptionsReachConfigureFunction", O3DB_TEST_FLAGS)
bool FO3DReceiverTypedConfigSavedOptionsTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverTypedConfigTest;
	FScopedTypedReceiverTransport Transport;

	// Settings as they load from GameUserSettings.ini or a LiveLink preset saved before WP-A1 PR 5a.
	FO3DReceiverSourceConfig Settings;
	Settings.TransportName = Transport.Name;
	Settings.TransportOptions.Add(Transport.UrlKey, TEXT("wss://saved.invalid"));
	const FString Token = TEXT("TYPED-RECEIVER-TOKEN-04bd");
	FO3DSecretStore::Get().Set(Transport.Name.ToString(), TEXT("default"), Transport.SecretKey, Token, EO3DSecretPersistence::Session);

	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(Settings);
	const FO3DTransportConfig Config = FO3DReceiverSourceTestAccessor::BuildTransportConfig(*Source);
	TestEqual(TEXT("The configure function ran once"), Transport.Seen->Calls, 1);
	TestEqual(TEXT("It saw the saved url"), Transport.Seen->Values.FindRef(Transport.UrlKey), FString(TEXT("wss://saved.invalid")));
	TestFalse(TEXT("It never saw the secret"), Transport.Seen->Values.Contains(Transport.SecretKey));
	TestTrue(TEXT("Its view carried the schema"), Transport.Seen->bHadSchema);
	TestEqual(TEXT("Uri from the view"), Config.Uri, FString(TEXT("wss://saved.invalid")));
	TestEqual(TEXT("Secret resolved into Config.Secrets"), Config.Secrets.FindRef(Transport.SecretKey), Token);
	TestTrue(TEXT("Config carries the schema"), Config.OptionSchema.IsValid());
	TestTrue(TEXT("A receiver has no Subject Name"), Config.SubjectName.IsEmpty());

	// Unset, the schema default reaches the transport.
	Settings.TransportOptions.Reset();
	const TSharedRef<FO3DReceiverSource> Defaulted = MakeShared<FO3DReceiverSource>(Settings);
	TestEqual(TEXT("Schema default through the view"), FO3DReceiverSourceTestAccessor::BuildTransportConfig(*Defaulted).Uri, FString(TEXT("wss://default.invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DReceiverTransportSwitchTest, "Open3DBroadcast.Receiver.TransportSwitch.KeepsOtherTransportsOptions", O3DB_TEST_FLAGS)
bool FO3DReceiverTransportSwitchTest::RunTest(const FString& Parameters)
{
	using namespace O3DReceiverTypedConfigTest;
	FScopedTypedReceiverTransport First;
	// Both transports are registered: an unregistered one's options are never kept (ADR 0004).
	FScopedTypedReceiverTransport Other;
	const FName Second = Other.Name;

	FO3DReceiverSourceConfig Settings;
	Settings.TransportName = First.Name;
	Settings.TransportOptions.Add(TEXT("host"), TEXT("10.4.5.6"));
	Settings.TransportOptions.Add(TEXT("port"), TEXT("9200"));
	Settings.TransportOptions.Add(First.SecretKey, TEXT("LEGACY-RECEIVER-SECRET-77aa"));

	TestTrue(TEXT("Switch reported"), O3DReceiver::SwitchTransport(Settings, Second));
	TestTrue(TEXT("Transport selected"), Settings.TransportName == Second);
	TestEqual(TEXT("The new transport starts empty"), Settings.TransportOptions.Num(), 0);
	Settings.TransportOptions.Add(TEXT("port"), TEXT("7100"));

	// The connection string a LiveLink preset saves carries only the selected transport's options.
	const FString ConnectionString = O3DReceiver::ExportConnectionString(Settings);
	TestTrue(TEXT("Connection string has the selected port"), ConnectionString.Contains(TEXT("7100")));
	TestFalse(TEXT("Connection string leaves out the other transport's options"), ConnectionString.Contains(TEXT("10.4.5.6")));
	TestFalse(TEXT("Connection string never has the secret"), ConnectionString.Contains(TEXT("LEGACY-RECEIVER-SECRET-77aa")));

	TestTrue(TEXT("Switch back reported"), O3DReceiver::SwitchTransport(Settings, First.Name));
	TestEqual(TEXT("Back: host restored"), Settings.TransportOptions.FindRef(TEXT("host")), FString(TEXT("10.4.5.6")));
	TestEqual(TEXT("Back: its own port"), Settings.TransportOptions.FindRef(TEXT("port")), FString(TEXT("9200")));
	TestFalse(TEXT("The secret was not put away"), Settings.TransportOptions.Contains(First.SecretKey));
	TestEqual(TEXT("The other transport's port is kept"), Settings.InactiveTransportOptions.FindRef(Second).Options.FindRef(TEXT("port")), FString(TEXT("7100")));
	TestFalse(TEXT("Same transport: no switch"), O3DReceiver::SwitchTransport(Settings, First.Name));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
