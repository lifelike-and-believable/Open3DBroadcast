// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 5c (ADR 0007 item 8, the rest of step 5; TRB-27, ADR 0004):
// - FO3DTransportConfig carries the registered transport name (FName) and the side
//   (EO3DTransportRole); the hosts and the built-in transports' configure functions set both.
// - A Secret schema entry declares its key secret and carries its environment variable; the
//   deprecated SecretOptionKeys and SecretEnvVars still count. Redaction and persistence are as
//   ADR 0004 says.
// - A Float field's Min and Max bound FO3DTransportOptionsView::GetDouble.
// - A field's Validate refuses options with InvalidConfig, before the sender component or the
//   receiver source creates the transport.
// Test-only transport names and keys; the fake transport of O3DTestFakes; no network.

#include "O3DTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#include "O3DReceiverSourceSettings.h"
#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "Testing/O3DReceiverTesting.h"
#include "Testing/O3DSenderTesting.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportRegistry.h"

namespace O3DTransportSchemaTest
{
	/** A descriptor with fake factories that count the instances they create. */
	struct FCounts
	{
		int32 Senders = 0;
		int32 Receivers = 0;
	};

	FO3DTransportDescriptor MakeCountingDescriptor(FName Name, const TSharedRef<FCounts>& Counts)
	{
		FO3DTransportDescriptor Descriptor;
		Descriptor.Name = Name;
		Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
		Descriptor.CreateSender = [Counts]() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>
		{
			++Counts->Senders;
			return MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>();
		};
		Descriptor.CreateReceiver = [Counts]() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>
		{
			++Counts->Receivers;
			return MakeShared<FO3DFakeReceiver, ESPMode::ThreadSafe>();
		};
		return Descriptor;
	}

	FO3DTransportOptionField MakeField(const FString& Key, EO3DTransportOptionType Type)
	{
		FO3DTransportOptionField Field;
		Field.Key = Key;
		Field.DisplayName = FText::FromString(Key);
		Field.Type = Type;
		return Field;
	}

	/** A Validate that refuses anything containing "bad". */
	bool RefuseBad(const FString& Value, FText& OutError)
	{
		if (Value.Contains(TEXT("bad")))
		{
			OutError = FText::FromString(TEXT("Use a value without 'bad'."));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportSchemaConfigNameRoleTest, "Open3DBroadcast.Shared.TypedConfig.ConfigCarriesTransportAndRole", O3DB_TEST_FLAGS)
bool FO3DTransportSchemaConfigNameRoleTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportSchemaTest;

	// The constructor and the debug string.
	const FO3DTransportConfig Constructed(TEXT("TCP"), EO3DTransportRole::Receiver);
	TestEqual(TEXT("Constructor sets the transport"), Constructed.Transport, FName(TEXT("TCP")));
	TestEqual(TEXT("Constructor sets the role"), Constructed.Role, EO3DTransportRole::Receiver);
	TestTrue(TEXT("Debug string names the role"), Constructed.ToDebugString().Contains(TEXT("Receiver")));
	TestEqual(TEXT("A default config is for the sender side"), FO3DTransportConfig().Role, EO3DTransportRole::Sender);
	TestTrue(TEXT("A default config has no transport"), FO3DTransportConfig().Transport.IsNone());

	// The hosts set the registered name and their side.
	const FName Name(*O3DTests::MakeUniqueName(TEXT("O3DSchemaNameRole")));
	const TSharedRef<FCounts> Counts = MakeShared<FCounts>();
	FO3DTransportRegistration Registration = FO3DTransportRegistry::Get().Register(MakeCountingDescriptor(Name, Counts));
	if (!TestTrue(TEXT("Test transport registered"), Registration.IsValid()))
	{
		return false;
	}

	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->SetTransportName(Name);
	const FO3DTransportConfig SenderConfig = FO3DSenderComponentTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("Sender config: registered name"), SenderConfig.Transport, Name);
	TestEqual(TEXT("Sender config: sender side"), SenderConfig.Role, EO3DTransportRole::Sender);

	FO3DReceiverSourceConfig SourceConfig;
	SourceConfig.TransportName = Name;
	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(SourceConfig);
	const FO3DTransportConfig ReceiverConfig = FO3DReceiverSourceTestAccessor::BuildTransportConfig(*Source);
	TestEqual(TEXT("Receiver config: registered name"), ReceiverConfig.Transport, Name);
	TestEqual(TEXT("Receiver config: receiver side"), ReceiverConfig.Role, EO3DTransportRole::Receiver);
	Registration.Reset();

	// Every built-in transport's configure functions keep the registered name and set the side
	// (TRB-27: TCP's used to be "sockets.tcp"). Transports that are not built are skipped.
	const FName BuiltIn[] = { TEXT("Loopback"), TEXT("TCP"), TEXT("UDP"), TEXT("NNG"), TEXT("MoQ") };
	for (const FName& Transport : BuiltIn)
	{
		const FO3DTransportDescriptorPtr Descriptor = FO3DTransportRegistry::Get().Find(Transport);
		if (!Descriptor.IsValid())
		{
			continue;
		}
		const TMap<FString, FString> NoOptions;
		if (Descriptor->ConfigureSender)
		{
			FO3DTransportConfig Config(Transport, EO3DTransportRole::Sender);
			Descriptor->ConfigureSender(FO3DTransportOptionsView(NoOptions, &Descriptor->SenderOptions.OptionSchema), Config);
			TestEqual(*FString::Printf(TEXT("%s sender: registered name"), *Transport.ToString()), Config.Transport, Transport);
			TestEqual(*FString::Printf(TEXT("%s sender: sender side"), *Transport.ToString()), Config.Role, EO3DTransportRole::Sender);
		}
		if (Descriptor->ConfigureReceiver)
		{
			FO3DTransportConfig Config(Transport, EO3DTransportRole::Receiver);
			Descriptor->ConfigureReceiver(FO3DTransportOptionsView(NoOptions, &Descriptor->ReceiverOptions.OptionSchema), Config);
			TestEqual(*FString::Printf(TEXT("%s receiver: registered name"), *Transport.ToString()), Config.Transport, Transport);
			TestEqual(*FString::Printf(TEXT("%s receiver: receiver side"), *Transport.ToString()), Config.Role, EO3DTransportRole::Receiver);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportSchemaSecretEnvVarTest, "Open3DBroadcast.Shared.OptionSchema.SecretEntryCarriesEnvVar", O3DB_TEST_FLAGS)
bool FO3DTransportSchemaSecretEnvVarTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportSchemaTest;

	// The declaration: Secret entries first, with their env var; the deprecated lists after.
	{
		FO3DTransportRoleOptions Options;
		FO3DTransportOptionField Token = MakeField(TEXT("schematest.token"), EO3DTransportOptionType::Secret);
		Token.SecretEnvVar = TEXT("O3DB_SCHEMATEST_TOKEN");
		Options.OptionSchema.Add(Token);
		FO3DTransportOptionField Other = MakeField(TEXT("schematest.other"), EO3DTransportOptionType::Secret);
		Options.OptionSchema.Add(Other);
		Options.OptionSchema.Add(MakeField(TEXT("schematest.url"), EO3DTransportOptionType::Url));
		// Deprecated inputs: a duplicate in another case, an env var the entry overrides, a key of their own.
		Options.SecretOptionKeys = { TEXT("SchemaTest.Token"), TEXT("schematest.legacy") };
		Options.SecretEnvVars.Add(TEXT("schematest.token"), TEXT("O3DB_SCHEMATEST_OLD"));
		Options.SecretEnvVars.Add(TEXT("schematest.other"), TEXT("O3DB_SCHEMATEST_OTHER"));
		Options.SecretEnvVars.Add(TEXT("schematest.legacy"), TEXT("O3DB_SCHEMATEST_LEGACY"));

		TArray<FString> Keys;
		TMap<FString, FString> EnvVars;
		Options.GetSecretDeclaration(Keys, EnvVars);
		TestEqual(TEXT("Three secret keys (the duplicate counted once)"), Keys.Num(), 3);
		TestTrue(TEXT("The entry's key"), Keys.Contains(TEXT("schematest.token")));
		TestTrue(TEXT("The second entry's key"), Keys.Contains(TEXT("schematest.other")));
		TestTrue(TEXT("The deprecated list's own key"), Keys.Contains(TEXT("schematest.legacy")));
		TestFalse(TEXT("A non-Secret entry is not secret"), Keys.Contains(TEXT("schematest.url")));
		TestEqual(TEXT("The entry's env var wins"), EnvVars.FindRef(TEXT("schematest.token")), FString(TEXT("O3DB_SCHEMATEST_TOKEN")));
		TestEqual(TEXT("An entry without one falls back to SecretEnvVars"), EnvVars.FindRef(TEXT("schematest.other")), FString(TEXT("O3DB_SCHEMATEST_OTHER")));
		TestEqual(TEXT("The deprecated key keeps its env var"), EnvVars.FindRef(TEXT("schematest.legacy")), FString(TEXT("O3DB_SCHEMATEST_LEGACY")));
	}

	// Through the registry, with only a schema entry: the env var resolves from the entry.
	const FName Name(*O3DTests::MakeUniqueName(TEXT("O3DSchemaSecret")));
	const FString SecretKey = Name.ToString().ToLower() + TEXT(".token");
	const FString UrlKey = Name.ToString().ToLower() + TEXT(".url");
	const FString EnvVar = TEXT("O3DB_SCHEMATEST_ENTRY_TOKEN");
	const TSharedRef<FCounts> Counts = MakeShared<FCounts>();
	FO3DTransportDescriptor Descriptor = MakeCountingDescriptor(Name, Counts);
	FO3DTransportOptionField Secret = MakeField(SecretKey, EO3DTransportOptionType::Secret);
	Secret.SecretEnvVar = EnvVar;
	Descriptor.SenderOptions.OptionSchema.Add(Secret);
	Descriptor.SenderOptions.OptionSchema.Add(MakeField(UrlKey, EO3DTransportOptionType::Url));
	FO3DTransportRegistration Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
	if (!TestTrue(TEXT("Test transport registered"), Registration.IsValid()))
	{
		return false;
	}

	TArray<FString> Keys;
	TMap<FString, FString> EnvVars;
	TestTrue(TEXT("Declaration found"), FO3DTransportRegistry::Get().GetSecretDeclaration(Name, EO3DTransportRole::Sender, Keys, EnvVars));
	TestTrue(TEXT("Declared from the entry alone"), Keys.Num() == 1 && Keys[0] == SecretKey);
	TestEqual(TEXT("Env var from the entry"), EnvVars.FindRef(SecretKey), EnvVar);

	// The store resolves it from that variable (a fake environment, so the process is untouched).
	const FString EnvValue = TEXT("ENTRY-ENV-SECRET-7d41");
	FO3DSecretStore Store([&EnvVar, &EnvValue](const FString& Variable) { return Variable == EnvVar ? EnvValue : FString(); });
	TMap<FString, FString> Resolved;
	Store.ResolveAll(Name.ToString(), FO3DSecretStore::DefaultProfile(), Keys, EnvVars, Resolved);
	TestEqual(TEXT("Resolved from the entry's env var"), Resolved.FindRef(SecretKey), EnvValue);

	// Persistence and redaction (ADR 0004), unchanged: the component treats the key as secret.
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	Component->SetTransportName(Name);
	const FString SessionValue = TEXT("ENTRY-SESSION-SECRET-2b9e");
	TestTrue(TEXT("The component sees the key as secret"), Component->IsTransportSecretKey(SecretKey));
	Component->SetTransportOption(SecretKey, SessionValue);
	Component->SetTransportOption(UrlKey, TEXT("wss://entry.invalid"));
	TestFalse(TEXT("The secret is not in the saved options"), Component->TransportOptions.Contains(SecretKey));
	TestTrue(TEXT("It is in the session store"), FO3DSecretStore::Get().HasSessionValue(Name.ToString(), FO3DSecretStore::DefaultProfile(), SecretKey));

	const FO3DTransportConfig Config = FO3DSenderComponentTestAccess::BuildTransportConfig(*Component);
	TestEqual(TEXT("The secret reaches Config.Secrets"), Config.Secrets.FindRef(SecretKey), SessionValue);
	TestFalse(TEXT("The secret is not in AdvancedParams"), Config.AdvancedParams.Contains(SecretKey));
	TestFalse(TEXT("The debug string does not show it"), Config.ToDebugString().Contains(SessionValue));
	TestTrue(TEXT("The debug string says it is set"), Config.ToDebugString().Contains(SecretKey + TEXT("=<set>")));

	FO3DSecretStore::Get().Clear(Name.ToString(), FO3DSecretStore::DefaultProfile(), SecretKey);
	Registration.Reset();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportSchemaFloatRangeTest, "Open3DBroadcast.Shared.OptionSchema.FloatHonoursMinAndMax", O3DB_TEST_FLAGS)
bool FO3DTransportSchemaFloatRangeTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportSchemaTest;

	FO3DTransportOptionSchema Schema;
	FO3DTransportOptionField Ranged = MakeField(TEXT("float.ranged"), EO3DTransportOptionType::Float);
	Ranged.Default = TEXT("1.25");
	Ranged.Min = 0.5;
	Ranged.Max = 2.0;
	Schema.Add(Ranged);
	FO3DTransportOptionField WideDefault = MakeField(TEXT("float.widedefault"), EO3DTransportOptionType::Float);
	WideDefault.Default = TEXT("9");
	WideDefault.Min = 0.0;
	WideDefault.Max = 1.0;
	Schema.Add(WideDefault);
	FO3DTransportOptionField Unbounded = MakeField(TEXT("float.unbounded"), EO3DTransportOptionType::Float);
	Unbounded.Default = TEXT("-3.5");
	Schema.Add(Unbounded);
	FO3DTransportOptionField IntField = MakeField(TEXT("int.ranged"), EO3DTransportOptionType::Int);
	IntField.Min = 1;
	IntField.Max = 10;
	Schema.Add(IntField);

	TMap<FString, FString> Values;
	const FO3DTransportOptionsView View(Values, &Schema);
	TestEqual(TEXT("Unset: the default"), View.GetDouble(TEXT("float.ranged")), 1.25);
	TestEqual(TEXT("A default outside the range is clamped too"), View.GetDouble(TEXT("float.widedefault")), 1.0);
	TestEqual(TEXT("No range (Max <= Min): the default as declared"), View.GetDouble(TEXT("float.unbounded")), -3.5);

	Values.Add(TEXT("float.ranged"), TEXT(" 3 "));
	TestEqual(TEXT("Above Max: Max"), View.GetDouble(TEXT("float.ranged")), 2.0);
	Values.Add(TEXT("float.ranged"), TEXT("0.1"));
	TestEqual(TEXT("Below Min: Min"), View.GetDouble(TEXT("float.ranged")), 0.5);
	Values.Add(TEXT("float.ranged"), TEXT("0.75"));
	TestEqual(TEXT("Inside: the value"), View.GetDouble(TEXT("float.ranged")), 0.75);
	Values.Add(TEXT("float.ranged"), TEXT("fast"));
	TestEqual(TEXT("Not a number: the default"), View.GetDouble(TEXT("float.ranged")), 1.25);
	Values.Add(TEXT("float.unbounded"), TEXT("1e6"));
	TestEqual(TEXT("No range: not clamped"), View.GetDouble(TEXT("float.unbounded")), 1.0e6);

	// Only Float fields are clamped by GetDouble; an Int range is the editor's shown unit.
	Values.Add(TEXT("int.ranged"), TEXT("50"));
	TestEqual(TEXT("Int field: GetDouble does not clamp"), View.GetDouble(TEXT("int.ranged")), 50.0);
	TestEqual(TEXT("Int field: GetInt does not clamp"), View.GetInt(TEXT("int.ranged")), 50);

	// The free getter keeps the caller's default and range, as before.
	TestEqual(TEXT("Free getter: caller's default"), O3DTransportOptions::GetDouble(FO3DTransportOptionsView(TMap<FString, FString>()), TEXT("float.ranged"), 7.0), 7.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DTransportSchemaValidateTest, "Open3DBroadcast.Shared.OptionSchema.ValidateGivesInvalidConfig", O3DB_TEST_FLAGS)
bool FO3DTransportSchemaValidateTest::RunTest(const FString& Parameters)
{
	using namespace O3DTransportSchemaTest;

	// ValidateOptions on its own.
	{
		FO3DTransportOptionSchema Schema;
		FO3DTransportOptionField Checked = MakeField(TEXT("check.value"), EO3DTransportOptionType::String);
		Checked.Validate = &RefuseBad;
		Schema.Add(Checked);
		FO3DTransportOptionField Hidden = MakeField(TEXT("check.hidden"), EO3DTransportOptionType::String);
		Hidden.Validate = &RefuseBad;
		Hidden.VisibleWhen = [](const TMap<FString, FString>&) { return false; };
		Schema.Add(Hidden);

		TMap<FString, FString> Values;
		const FO3DTransportOptionsView View(Values, &Schema);
		TestTrue(TEXT("Unset: Ok"), O3DTransportOptions::ValidateOptions(View).IsOk());
		Values.Add(TEXT("check.value"), TEXT("good"));
		TestTrue(TEXT("Accepted: Ok"), O3DTransportOptions::ValidateOptions(View).IsOk());
		Values.Add(TEXT("check.hidden"), TEXT("bad-but-hidden"));
		TestTrue(TEXT("A hidden row is not checked"), O3DTransportOptions::ValidateOptions(View).IsOk());

		Values.Add(TEXT("check.value"), TEXT("  very-bad-value  "));
		const FO3DTransportResult Result = O3DTransportOptions::ValidateOptions(View);
		TestEqual(TEXT("Refused: InvalidConfig"), Result.Code, EO3DTransportError::InvalidConfig);
		TestTrue(TEXT("The message names the key"), Result.Message.Contains(TEXT("'check.value'")));
		TestTrue(TEXT("The message has the validator's sentence"), Result.Message.Contains(TEXT("Use a value without 'bad'.")));
		TestFalse(TEXT("The message does not repeat the value"), Result.Message.Contains(TEXT("very-bad-value")));

		FText Error;
		TestFalse(TEXT("ValidateOptionValue refuses it too"), O3DTransportOptions::ValidateOptionValue(Checked, TEXT("bad"), Error));
		TestFalse(TEXT("With the error"), Error.IsEmpty());
		TestTrue(TEXT("An empty value is never refused"), O3DTransportOptions::ValidateOptionValue(Checked, TEXT("   "), Error));
		TestTrue(TEXT("Without a schema: Ok"), O3DTransportOptions::ValidateOptions(FO3DTransportOptionsView(Values)).IsOk());
	}

	// The hosts: nothing is created while the options are refused.
	const FName Name(*O3DTests::MakeUniqueName(TEXT("O3DSchemaValidate")));
	const FString Key = Name.ToString().ToLower() + TEXT(".value");
	const TSharedRef<FCounts> Counts = MakeShared<FCounts>();
	FO3DTransportDescriptor Descriptor = MakeCountingDescriptor(Name, Counts);
	FO3DTransportOptionField Checked = MakeField(Key, EO3DTransportOptionType::String);
	Checked.Validate = &RefuseBad;
	Descriptor.SenderOptions.OptionSchema.Add(Checked);
	Descriptor.ReceiverOptions.OptionSchema.Add(Checked);
	FO3DTransportRegistration Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
	if (!TestTrue(TEXT("Test transport registered"), Registration.IsValid()))
	{
		return false;
	}

	// Sender component: the controller refuses, capture still runs (control-only here).
	UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage());
	// StartCapture only starts a transport when the component creates its own (as in the lifetime test).
	Component->bAutoCreateTransport = true;
	Component->bEnableAudio = false;
	Component->bAllowControlOnly = true;
	Component->SetTransportName(Name);
	Component->SetTransportOption(Key, TEXT("bad"));
	// Control-only capture has no mesh: each of the two StartCapture calls below says so.
	AddExpectedError(TEXT("No TargetMesh set"), EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedError(FString::Printf(TEXT("Sender transport '%s' not started"), *Name.ToString()), EAutomationExpectedMessageFlags::Contains, 1);
	Component->StartCapture();
	const FO3DTransportResult SenderResult = Component->GetLastTransportResult();
	TestEqual(TEXT("Sender: InvalidConfig"), SenderResult.Code, EO3DTransportError::InvalidConfig);
	TestTrue(TEXT("Sender: the message names the key"), SenderResult.Message.Contains(Key));
	TestEqual(TEXT("Sender: no sender was created"), Counts->Senders, 0);
	Component->StopCapture();

	Component->SetTransportOption(Key, TEXT("good"));
	Component->StartCapture();
	TestTrue(TEXT("Sender: accepted options start"), Component->GetLastTransportResult().IsOk());
	TestEqual(TEXT("Sender: one sender created"), Counts->Senders, 1);
	Component->StopCapture();

	// Receiver source.
	FO3DReceiverSourceConfig SourceConfig;
	SourceConfig.TransportName = Name;
	SourceConfig.TransportOptions.Add(Key, TEXT("bad"));
	const TSharedRef<FO3DReceiverSource> Source = MakeShared<FO3DReceiverSource>(SourceConfig);
	AddExpectedError(FString::Printf(TEXT("Receiver transport '%s' not started"), *Name.ToString()), EAutomationExpectedMessageFlags::Contains, 1);
	TestFalse(TEXT("Receiver: StartTransport refuses"), FO3DReceiverSourceTestAccessor::StartTransport(*Source));
	TestEqual(TEXT("Receiver: InvalidConfig"), FO3DReceiverSourceTestAccessor::GetLastTransportResult(*Source).Code, EO3DTransportError::InvalidConfig);
	TestTrue(TEXT("Receiver: the status says why"), FO3DReceiverSourceTestAccessor::GetSourceStatus(*Source).ToString().Contains(TEXT("Use a value without 'bad'.")));
	TestEqual(TEXT("Receiver: no receiver was created"), Counts->Receivers, 0);
	FO3DReceiverSourceTestAccessor::StopTransport(*Source);

	Registration.Reset();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
