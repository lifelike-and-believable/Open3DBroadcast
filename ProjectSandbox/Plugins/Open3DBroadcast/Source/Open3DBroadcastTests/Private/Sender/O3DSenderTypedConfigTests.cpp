// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 5a (ADR 0007 items 4 and 8; SHR-36, SND-35): the sender component and typed config.
// - The options a component has saved reach the transport's configure function through an
//   FO3DTransportOptionsView with the transport's schema; secrets never do (ADR 0004); the
//   component's Subject Name arrives in FO3DTransportConfig::SubjectName.
// - A configure function registered through the deprecated customization still gets the component.
// - Switching the transport (SetTransportName and a Details-panel edit) keeps the other
//   transport's options and restores them when switching back (SND-35).
// Test-only transport names and keys under the process-wide registry; no transport module, no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "O3DSenderTransportCustomization.h"
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
		bool bLegacyGotComponent = false;
	};

	/**
	 * Registers a legacy-edited descriptor (no factory needed) for a unique name, with a schema,
	 * one secret key and a new-signature ConfigureSender; removes it and the secret on exit.
	 */
	struct FScopedTypedTransport
	{
		FName Name;
		FString UrlKey;
		FString CountKey;
		FString SecretKey;
		TSharedRef<FSeen> Seen = MakeShared<FSeen>();

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
			FO3DTransportRegistry::Get().EditLegacyDescriptor(Name, [&](FO3DTransportDescriptor& Descriptor)
			{
				Descriptor.SenderOptions.SecretOptionKeys.Add(SecretKey);
				Descriptor.SenderOptions.OptionSchema = Schema;
				Descriptor.ConfigureSender = [Record, Url_](const FO3DTransportOptionsView& Options, FO3DTransportConfig& Config)
				{
					++Record->Calls;
					Record->Values = Options.GetValues();
					Record->bHadSchema = Options.GetSchema() != nullptr;
					Record->SubjectName = Config.SubjectName;
					Config.Uri = Options.GetString(Url_);
				};
			});
		}

		~FScopedTypedTransport()
		{
			FO3DSecretStore::Get().Clear(Name.ToString(), TEXT("default"), SecretKey);
			FO3DTransportRegistry::Get().EditLegacyDescriptor(Name, [](FO3DTransportDescriptor& Descriptor)
			{
				Descriptor.ConfigureSender = FO3DSenderConfigureFunction();
				Descriptor.SenderOptions = FO3DTransportRoleOptions();
			});
		}
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DSenderTypedConfigLegacyTest, "Open3DBroadcast.Sender.TypedConfig.DeprecatedConfigureGetsComponent", O3DB_TEST_FLAGS)
bool FO3DSenderTypedConfigLegacyTest::RunTest(const FString& Parameters)
{
	// A transport built against the previous release registers a component-taking function
	// through the deprecated customization; the shim adapts it and hands it the component.
	const FName Name(*O3DTests::MakeUniqueName(TEXT("O3DTypedSenderLegacy")));
	const TSharedRef<O3DSenderTypedConfigTest::FSeen> Seen = MakeShared<O3DSenderTypedConfigTest::FSeen>();
	const UO3DSenderComponent* Expected = nullptr;

	FO3DSenderTransportCustomization Customization;
	Customization.ConfigureTransport = [Seen, &Expected](const UO3DSenderComponent* Component, FO3DTransportConfig& Config)
	{
		++Seen->Calls;
		Seen->bLegacyGotComponent = Component != nullptr && Component == Expected;
		Config.StreamId = Component ? Component->SubjectName : FString(TEXT("no-component"));
	};
	O3DSender::RegisterTransportCustomization(Name, MoveTemp(Customization));

	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Expected = Component;
	Component->SetTransportName(Name);
	Component->SubjectName = TEXT("LegacySubject");
	const FO3DTransportConfig Config = FO3DSenderComponentTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("The legacy function ran once"), Seen->Calls, 1);
	TestTrue(TEXT("It got the component being configured"), Seen->bLegacyGotComponent);
	TestEqual(TEXT("It could read the component"), Config.StreamId, FString(TEXT("LegacySubject")));

	// Called outside a component (as a caller of the descriptor might), it gets null, not a stale one.
	const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(Name);
	if (TestTrue(TEXT("Descriptor has the adapted function"), Descriptor.IsValid() && static_cast<bool>(Descriptor->ConfigureSender)))
	{
		FO3DTransportConfig Direct;
		Descriptor->ConfigureSender(FO3DTransportOptionsView(), Direct);
		TestEqual(TEXT("No component outside BuildTransportConfig"), Direct.StreamId, FString(TEXT("no-component")));
	}

	O3DSender::UnregisterTransportCustomization(Name);
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
