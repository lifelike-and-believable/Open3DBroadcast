// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 5a (ADR 0007 items 4 and 8; SHR-36, SND-35): the sender component and typed config.
// - The options a component has saved reach the transport's configure function through an
//   FO3DTransportOptionsView with the transport's schema; secrets never do (ADR 0004); the
//   component's Subject Name arrives in FO3DTransportConfig::SubjectName.
// - Switching the transport (SetTransportName and a Details-panel edit) keeps the other
//   transport's options and restores them when switching back (SND-35).
// Test-only transport names and keys under the process-wide registry; no transport module, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DSenderTypedConfigTest
{
	/** What the configure function saw. */
	struct FSeen
	{
		int32 Calls = 0;
		TMap<FString, FString> Values;
		bool bHadSchema = false;
		FString SubjectName;
	};

	/**
	 * Registers a descriptor for a unique name: a fake sender factory, a schema with a Secret
	 * entry, and ConfigureSender. Unregisters it and clears the secret on exit.
	 */
	struct FScopedTypedTransport
	{
		FName Name;
		FString UrlKey;
		FString CountKey;
		FString SecretKey;
		TSharedRef<FSeen> Seen = MakeShared<FSeen>();
		FO3DTransportRegistration Registration;

		FScopedTypedTransport()
			: Name(*O3DTests::MakeUniqueName(TEXT("O3DTypedSender")))
		{
			const FString Prefix = Name.ToString().ToLower();
			UrlKey = Prefix + TEXT(".url");
			CountKey = Prefix + TEXT(".count");
			SecretKey = Prefix + TEXT(".token");

			FO3DTransportOptionSchema Schema;
			FO3DTransportOptionField Url;
			Url.Key = UrlKey;
			Url.Type = EO3DTransportOptionType::Url;
			Schema.Add(Url);
			FO3DTransportOptionField Count;
			Count.Key = CountKey;
			Count.Type = EO3DTransportOptionType::Int;
			Count.Default = TEXT("12");
			Schema.Add(Count);
			FO3DTransportOptionField Secret;
			Secret.Key = SecretKey;
			Secret.Type = EO3DTransportOptionType::Secret;
			Schema.Add(Secret);

			const TSharedRef<FSeen> Record = Seen;
			const FString Url_ = UrlKey;
			FO3DTransportDescriptor Descriptor;
			Descriptor.Name = Name;
			Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
			Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>(); };
			Descriptor.SenderOptions.OptionSchema = Schema;
			Descriptor.ConfigureSender = [Record, Url_](const FO3DTransportOptionsView& Options, FO3DTransportConfig& Config)
			{
				++Record->Calls;
				Record->Values = Options.GetValues();
				Record->bHadSchema = Options.GetSchema() != nullptr;
				Record->SubjectName = Config.SubjectName;
				Config.Uri = Options.GetString(Url_);
			};
			Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
		}

		~FScopedTypedTransport()
		{
			FO3DSecretStore::Get().Clear(Name.ToString(), TEXT("default"), SecretKey);
			Registration.Reset();
		}

		FScopedTypedTransport(const FScopedTypedTransport&) = delete;
		FScopedTypedTransport& operator=(const FScopedTypedTransport&) = delete;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTypedConfigSavedOptionsTest, "Open3DBroadcast.Sender.TypedConfig.SavedOptionsReachConfigureFunction", O3DB_TEST_FLAGS)
bool FO3DSenderTypedConfigSavedOptionsTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderTypedConfigTest;
	FScopedTypedTransport Transport;

	// A component as it loads from an asset saved before WP-A1 PR 5a: the transport name and its
	// namespaced option map are the saved data; FO3DTransportConfig was never saved.
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->SetTransportName(Transport.Name);
	Component->SubjectName = TEXT("HeroSubject");
	Component->TransportOptions.Add(Transport.UrlKey, TEXT("  wss://saved.invalid  "));
	Component->TransportOptions.Add(Transport.CountKey, TEXT("30"));
	const FString Token = TEXT("TYPED-SENDER-TOKEN-91c2");
	FO3DSecretStore::Get().Set(Transport.Name.ToString(), TEXT("default"), Transport.SecretKey, Token, EO3DSecretPersistence::Session);

	const FO3DTransportConfig Config = FO3DSenderComponentTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("The configure function ran once"), Transport.Seen->Calls, 1);
	TestEqual(TEXT("It saw the saved url"), Transport.Seen->Values.FindRef(Transport.UrlKey), FString(TEXT("  wss://saved.invalid  ")));
	TestEqual(TEXT("It saw the saved count"), Transport.Seen->Values.FindRef(Transport.CountKey), FString(TEXT("30")));
	TestFalse(TEXT("It never saw the secret"), Transport.Seen->Values.Contains(Transport.SecretKey));
	TestTrue(TEXT("Its view carried the schema"), Transport.Seen->bHadSchema);
	TestEqual(TEXT("It saw the Subject Name"), Transport.Seen->SubjectName, FString(TEXT("HeroSubject")));

	TestEqual(TEXT("Uri from the view, trimmed"), Config.Uri, FString(TEXT("wss://saved.invalid")));
	TestEqual(TEXT("Secret resolved into Config.Secrets"), Config.Secrets.FindRef(Transport.SecretKey), Token);
	TestFalse(TEXT("Secret not in AdvancedParams"), Config.AdvancedParams.Contains(Transport.SecretKey));
	TestTrue(TEXT("Config carries the schema"), Config.OptionSchema.IsValid());
	TestEqual(TEXT("Typed read through the config"), Config.GetOptions().GetInt(Transport.CountKey), 30);
	TestEqual(TEXT("Config.SubjectName"), Config.SubjectName, FString(TEXT("HeroSubject")));

	// Unset, the schema's default applies.
	Component->TransportOptions.Remove(Transport.CountKey);
	const FO3DTransportConfig Defaulted = FO3DSenderComponentTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("Schema default through the config"), Defaulted.GetOptions().GetInt(Transport.CountKey), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTransportSwitchTest, "Open3DBroadcast.Sender.TransportSwitch.KeepsOtherTransportsOptions", O3DB_TEST_FLAGS)
bool FO3DSenderTransportSwitchTest::RunTest(const FString& Parameters)
{
	using namespace O3DSenderTypedConfigTest;
	FScopedTypedTransport First;
	// Both transports are registered: an unregistered one's options are never kept (ADR 0004).
	FScopedTypedTransport Other;
	const FName Second = Other.Name;

	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->SetTransportName(First.Name);
	Component->TransportOptions.Add(TEXT("host"), TEXT("10.1.2.3"));
	Component->TransportOptions.Add(TEXT("port"), TEXT("9100"));
	// What a pre-ADR 0004 asset could hold; a switch must not put it away.
	Component->TransportOptions.Add(First.SecretKey, TEXT("LEGACY-SECRET-5e1f"));

	// Runtime path.
	Component->SetTransportName(Second);
	TestEqual(TEXT("The new transport starts with its own (empty) options"), Component->TransportOptions.Num(), 0);
	Component->TransportOptions.Add(TEXT("port"), TEXT("7000"));

	Component->SetTransportName(First.Name);
	TestEqual(TEXT("Back: host restored"), Component->TransportOptions.FindRef(TEXT("host")), FString(TEXT("10.1.2.3")));
	TestEqual(TEXT("Back: its own port, not the other transport's"), Component->TransportOptions.FindRef(TEXT("port")), FString(TEXT("9100")));
	TestFalse(TEXT("The secret was not put away, so it is not restored"), Component->TransportOptions.Contains(First.SecretKey));
	for (const TPair<FName, FO3DTransportOptionSet>& Kept : Component->InactiveTransportOptions)
	{
		TestFalse(TEXT("No secret in the saved inactive options"), Kept.Value.Options.Contains(First.SecretKey));
	}
	TestEqual(TEXT("The other transport's port is kept"), Component->InactiveTransportOptions.FindRef(Second).Options.FindRef(TEXT("port")), FString(TEXT("7000")));

#if WITH_EDITOR
	// Details-panel path: PreEditChange, the change, PostEditChangeProperty.
	FO3DSenderComponentTestAccess::EditTransportName(*Component, Second);
	TestEqual(TEXT("Panel switch: the other transport's options come back"), Component->TransportOptions.FindRef(TEXT("port")), FString(TEXT("7000")));
	TestFalse(TEXT("Panel switch: no host from the first transport"), Component->TransportOptions.Contains(TEXT("host")));
	FO3DSenderComponentTestAccess::EditTransportName(*Component, First.Name);
	TestEqual(TEXT("Panel switch back: host restored"), Component->TransportOptions.FindRef(TEXT("host")), FString(TEXT("10.1.2.3")));
	TestEqual(TEXT("Panel switch back: port restored"), Component->TransportOptions.FindRef(TEXT("port")), FString(TEXT("9100")));
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
