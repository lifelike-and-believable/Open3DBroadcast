// Copyright Lifelike & Believable. All Rights Reserved.

// WP-F7 (ADR 0010 §5 and §8; TRB-45, SND-35): the generic transport options panel of the
// Open3DBroadcastEditor module.
// - Building a panel reads the object and never writes to it: the option map is unchanged and no
//   transaction is recorded, for a test schema and for every registered transport.
// - One commit is one transaction, and undo restores the previous option map.
// - A commit that changes nothing records nothing; Int values are clamped and scaled.
// - Secret fields write to FO3DSecretStore only, never to the option map or the undo buffer.
// - A panel whose object is gone does nothing.
//
// Uses test-only transport names and keys. No network. Needs the editor (GEditor->Trans).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Editor.h"
#include "Editor/Transactor.h"
#include "UObject/Package.h"

#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DSecretStore.h"
#include "O3DSenderComponent.h"
#include "O3DTestFakes.h"
#include "O3DTransportOptionSchema.h"
#include "O3DTransportOptionTarget.h"
#include "SO3DTransportOptionsPanel.h"

namespace O3DPanelTestUtil
{
	static const TCHAR* const TransportName = TEXT("O3DPanelTest");
	static const TCHAR* const HostKey = TEXT("o3dpaneltest.host");
	static const TCHAR* const PortKey = TEXT("o3dpaneltest.port");
	static const TCHAR* const QueueKey = TEXT("o3dpaneltest.queue");
	static const TCHAR* const FlagKey = TEXT("o3dpaneltest.flag");
	static const TCHAR* const ModeKey = TEXT("o3dpaneltest.mode");
	static const TCHAR* const TopicKey = TEXT("o3dpaneltest.topic");
	static const TCHAR* const SecretKey = TEXT("o3dpaneltest.token");

	/** One field of every type, with defaults, a range, a unit scale and a VisibleWhen. */
	static FO3DTransportOptionSchema MakeSchema()
	{
		FO3DTransportOptionSchema Schema;

		FO3DTransportOptionField Host;
		Host.Key = HostKey;
		Host.DisplayName = FText::FromString(TEXT("Host"));
		Host.Type = EO3DTransportOptionType::String;
		Host.Default = TEXT("127.0.0.1");
		Schema.Add(Host);

		FO3DTransportOptionField Port;
		Port.Key = PortKey;
		Port.DisplayName = FText::FromString(TEXT("Port"));
		Port.Type = EO3DTransportOptionType::Int;
		Port.Default = TEXT("17700");
		Port.Min = 1;
		Port.Max = 65535;
		Schema.Add(Port);

		FO3DTransportOptionField Queue;
		Queue.Key = QueueKey;
		Queue.DisplayName = FText::FromString(TEXT("Queue (MiB)"));
		Queue.Type = EO3DTransportOptionType::Int;
		Queue.Default = TEXT("4194304");
		Queue.StoredUnitScale = 1024 * 1024;
		Queue.Min = 1;
		Queue.Max = 512;
		Schema.Add(Queue);

		FO3DTransportOptionField Flag;
		Flag.Key = FlagKey;
		Flag.DisplayName = FText::FromString(TEXT("Flag"));
		Flag.Type = EO3DTransportOptionType::Bool;
		Flag.Default = TEXT("false");
		Schema.Add(Flag);

		FO3DTransportOptionField Mode;
		Mode.Key = ModeKey;
		Mode.DisplayName = FText::FromString(TEXT("Mode"));
		Mode.Type = EO3DTransportOptionType::Enum;
		Mode.Default = TEXT("a");
		FO3DTransportOptionEnumValue ChoiceA;
		ChoiceA.Value = TEXT("a");
		ChoiceA.DisplayName = FText::FromString(TEXT("A"));
		FO3DTransportOptionEnumValue ChoiceB;
		ChoiceB.Value = TEXT("b");
		ChoiceB.DisplayName = FText::FromString(TEXT("B"));
		Mode.EnumValues = { ChoiceA, ChoiceB };
		Schema.Add(Mode);

		FO3DTransportOptionField Topic;
		Topic.Key = TopicKey;
		Topic.DisplayName = FText::FromString(TEXT("Topic"));
		Topic.Type = EO3DTransportOptionType::String;
		Topic.VisibleWhen = O3DTransportOptions::VisibleWhenEquals(ModeKey, TEXT("b"), TEXT("a"));
		Schema.Add(Topic);

		FO3DTransportOptionField Secret;
		Secret.Key = SecretKey;
		Secret.DisplayName = FText::FromString(TEXT("Token"));
		Secret.Type = EO3DTransportOptionType::Secret;
		Schema.Add(Secret);

		return Schema;
	}

	/**
	 * Registers the test transport on both sides (fake factories and the schema, whose Secret entry
	 * declares the secret key); unregisters it on exit.
	 */
	struct FScopedPanelTestTransport
	{
		FScopedPanelTestTransport()
		{
			FO3DTransportDescriptor Descriptor;
			Descriptor.Name = TransportName;
			Descriptor.OwningModule = TEXT("Open3DBroadcastTests");
			Descriptor.CreateSender = []() -> TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeSender, ESPMode::ThreadSafe>(); };
			Descriptor.CreateReceiver = []() -> TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe> { return MakeShared<FO3DFakeReceiver, ESPMode::ThreadSafe>(); };
			Descriptor.SenderOptions.OptionSchema = MakeSchema();
			Descriptor.ReceiverOptions.OptionSchema = MakeSchema();
			Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));

			ClearStore();
		}

		~FScopedPanelTestTransport()
		{
			ClearStore();
			Registration.Reset();
		}

		FScopedPanelTestTransport(const FScopedPanelTestTransport&) = delete;
		FScopedPanelTestTransport& operator=(const FScopedPanelTestTransport&) = delete;

		FO3DTransportRegistration Registration;

		static void ClearStore()
		{
			FO3DSecretStore::Get().Clear(TransportName, FO3DSecretStore::DefaultProfile(), SecretKey);
		}
	};

	/** A transactional sender component in the transient package, on TransportName, with no options. */
	static UO3DSenderComponent* NewSender(FName InTransport)
	{
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage(), NAME_None, RF_Transactional);
		Component->TransportName = InTransport;
		Component->TransportOptions.Reset();
		return Component;
	}

	/** A transactional receiver settings object in the transient package, on TransportName, with no options. */
	static UO3DReceiverSettingsObject* NewReceiverSettings(FName InTransport)
	{
		UO3DReceiverSettingsObject* Settings = NewObject<UO3DReceiverSettingsObject>(GetTransientPackage(), NAME_None, RF_Transactional);
		Settings->Settings.TransportName = InTransport;
		Settings->Settings.TransportOptions.Reset();
		return Settings;
	}

	static bool SameOptions(const TMap<FString, FString>& A, const TMap<FString, FString>& B)
	{
		return A.OrderIndependentCompareEqual(B);
	}

	/** Undo buffer length, or INDEX_NONE without an editor transactor. */
	static int32 GetUndoQueueLength()
	{
		return (GEditor && GEditor->Trans) ? GEditor->Trans->GetQueueLength() : INDEX_NONE;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelConstructTest, "Open3DBroadcast.Editor.OptionsPanel.ConstructDoesNotModifyTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelConstructTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelTestUtil;
	const FScopedPanelTestTransport Scope;

	const int32 QueueBefore = GetUndoQueueLength();
	if (!TestNotEqual(TEXT("The editor has a transaction buffer"), QueueBefore, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	// Sender: an empty map stays empty; the defaults are hints, not writes (TRB-45).
	UO3DSenderComponent* Component = NewSender(TransportName);
	TSharedRef<SO3DTransportOptionsPanel> SenderPanel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FO3DSenderOptionTarget>(Component))
		.Schema(MakeSchema());
	TestEqual(TEXT("Sender options untouched by Construct"), Component->TransportOptions.Num(), 0);

	// A value a user chose that equals another transport's default is kept (TRB-45: the old
	// sockets panel reset 17800 to 17700).
	Component->TransportOptions.Add(PortKey, TEXT("17800"));
	const TMap<FString, FString> Before = Component->TransportOptions;
	TSharedRef<SO3DTransportOptionsPanel> SecondPanel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FO3DSenderOptionTarget>(Component))
		.Schema(MakeSchema());
	TestTrue(TEXT("Sender options with a value untouched by Construct"), SameOptions(Component->TransportOptions, Before));
	TestEqual(TEXT("Second panel shows the same rows"), SecondPanel->GetNumVisibleFields(), SenderPanel->GetNumVisibleFields());

	// Receiver.
	UO3DReceiverSettingsObject* Settings = NewReceiverSettings(TransportName);
	TSharedRef<SO3DTransportOptionsPanel> ReceiverPanel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FO3DReceiverOptionTarget>(Settings))
		.Schema(MakeSchema());
	TestEqual(TEXT("Receiver options untouched by Construct"), Settings->Settings.TransportOptions.Num(), 0);
	TestEqual(TEXT("Receiver panel shows the same rows"), ReceiverPanel->GetNumVisibleFields(), SenderPanel->GetNumVisibleFields());

	TestEqual(TEXT("Construct records no transaction"), GetUndoQueueLength(), QueueBefore);

	// VisibleWhen: the topic row shows only in mode "b"; the default mode is "a".
	TestEqual(TEXT("All rows but the topic are visible"), SenderPanel->GetNumVisibleFields(), MakeSchema().Num() - 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelTransactionTest, "Open3DBroadcast.Editor.OptionsPanel.CommitIsOneUndoableTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelTransactionTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelTestUtil;
	const FScopedPanelTestTransport Scope;

	const int32 QueueStart = GetUndoQueueLength();
	if (!TestNotEqual(TEXT("The editor has a transaction buffer"), QueueStart, static_cast<int32>(INDEX_NONE)))
	{
		return false;
	}

	UO3DSenderComponent* Component = NewSender(TransportName);
	TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FO3DSenderOptionTarget>(Component))
		.Schema(MakeSchema());

	// One commit, one transaction (SND-35).
	TestTrue(TEXT("Port commit changes the component"), Panel->CommitFieldValue(PortKey, TEXT("1234")));
	TestEqual(TEXT("Port stored"), Component->GetTransportOption(PortKey), FString(TEXT("1234")));
	TestEqual(TEXT("One transaction for one commit"), GetUndoQueueLength(), QueueStart + 1);

	// The same value again records nothing (a spin box released on its old value, a focus change).
	TestFalse(TEXT("Unchanged commit reports no change"), Panel->CommitFieldValue(PortKey, TEXT("1234")));
	TestEqual(TEXT("No transaction for an unchanged commit"), GetUndoQueueLength(), QueueStart + 1);

	// Undo restores the previous option map (the key was unset).
	TestTrue(TEXT("Undo succeeds"), GEditor->UndoTransaction());
	TestFalse(TEXT("Undo removed the port"), Component->TransportOptions.Contains(PortKey));

	// Int: clamped to the range, and scaled to the stored unit.
	Panel->CommitFieldValue(PortKey, TEXT("999999"));
	TestEqual(TEXT("Port clamped to Max"), Component->GetTransportOption(PortKey), FString(TEXT("65535")));
	Panel->CommitFieldValue(QueueKey, TEXT("4"));
	TestEqual(TEXT("MiB stored as bytes"), Component->GetTransportOption(QueueKey), FString(TEXT("4194304")));
	TestFalse(TEXT("Non-numeric Int rejected"), Panel->CommitFieldValue(PortKey, TEXT("abc")));

	// String: trimmed. Enum: only declared values. Bool: normalised.
	Panel->CommitFieldValue(HostKey, TEXT("  10.0.0.1  "));
	TestEqual(TEXT("Host trimmed"), Component->GetTransportOption(HostKey), FString(TEXT("10.0.0.1")));
	TestFalse(TEXT("Undeclared enum value rejected"), Panel->CommitFieldValue(ModeKey, TEXT("zzz")));
	TestTrue(TEXT("Declared enum value accepted"), Panel->CommitFieldValue(ModeKey, TEXT("b")));
	Panel->CommitFieldValue(FlagKey, TEXT("1"));
	TestEqual(TEXT("Bool normalised"), Component->GetTransportOption(FlagKey), FString(TEXT("true")));

	// VisibleWhen follows the committed mode.
	TestEqual(TEXT("Topic row visible in mode b"), Panel->GetNumVisibleFields(), MakeSchema().Num());

	// An empty commit removes the key, so the transport default applies again.
	Panel->CommitFieldValue(HostKey, TEXT(""));
	TestFalse(TEXT("Empty commit removes the key"), Component->TransportOptions.Contains(HostKey));

	// Receiver side: same contract.
	UO3DReceiverSettingsObject* Settings = NewReceiverSettings(TransportName);
	TSharedRef<SO3DTransportOptionsPanel> ReceiverPanel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FO3DReceiverOptionTarget>(Settings))
		.Schema(MakeSchema());
	const int32 QueueBeforeReceiver = GetUndoQueueLength();
	TestTrue(TEXT("Receiver commit changes the settings"), ReceiverPanel->CommitFieldValue(PortKey, TEXT("4321")));
	TestEqual(TEXT("Receiver: one transaction"), GetUndoQueueLength(), QueueBeforeReceiver + 1);
	TestTrue(TEXT("Receiver undo succeeds"), GEditor->UndoTransaction());
	TestFalse(TEXT("Receiver undo removed the port"), Settings->Settings.TransportOptions.Contains(PortKey));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelSecretTest, "Open3DBroadcast.Editor.OptionsPanel.SecretNeverEntersOptionMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelSecretTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelTestUtil;
	const FScopedPanelTestTransport Scope;

	UO3DSenderComponent* Component = NewSender(TransportName);
	TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FO3DSenderOptionTarget>(Component))
		.Schema(MakeSchema());

	const int32 QueueBefore = GetUndoQueueLength();
	Panel->CommitSecretValue(SecretKey, TEXT("panel-test-secret"));
	TestFalse(TEXT("Secret not in TransportOptions"), Component->TransportOptions.Contains(SecretKey));
	TestTrue(TEXT("Secret set for this session"), Component->GetTransportSecretStatus(SecretKey).Source == EO3DSecretSource::Session);
	TestEqual(TEXT("A secret records no transaction"), GetUndoQueueLength(), QueueBefore);
	TestFalse(TEXT("A secret cannot be committed as an option"), Panel->CommitFieldValue(SecretKey, TEXT("other")));
	TestFalse(TEXT("Still not in TransportOptions"), Component->TransportOptions.Contains(SecretKey));

	// Receiver: the secret goes to the store under the transport and the default profile.
	UO3DReceiverSettingsObject* Settings = NewReceiverSettings(TransportName);
	FScopedPanelTestTransport::ClearStore();
	TSharedRef<SO3DTransportOptionsPanel> ReceiverPanel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FO3DReceiverOptionTarget>(Settings))
		.Schema(MakeSchema());
	ReceiverPanel->CommitSecretValue(SecretKey, TEXT("panel-test-secret"));
	TestFalse(TEXT("Receiver secret not in TransportOptions"), Settings->Settings.TransportOptions.Contains(SecretKey));
	TestTrue(TEXT("Receiver secret in the store"), FO3DSecretStore::Get().HasSessionValue(TransportName, FO3DSecretStore::DefaultProfile(), SecretKey));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelInvalidTargetTest, "Open3DBroadcast.Editor.OptionsPanel.InvalidTargetDoesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelInvalidTargetTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelTestUtil;
	const FScopedPanelTestTransport Scope;

	// A target whose weak pointer does not resolve, as after garbage collection or the end of PIE.
	const TSharedRef<FO3DSenderOptionTarget> Target = MakeShared<FO3DSenderOptionTarget>(nullptr);
	TestFalse(TEXT("Target reports invalid"), Target->IsValid());

	const int32 QueueBefore = GetUndoQueueLength();
	TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
		.Target(Target)
		.Schema(MakeSchema());
	TestFalse(TEXT("Commit on a missing object does nothing"), Panel->CommitFieldValue(PortKey, TEXT("1234")));
	Panel->CommitSecretValue(SecretKey, TEXT("ignored"));
	TestEqual(TEXT("No transaction for a missing object"), GetUndoQueueLength(), QueueBefore);
	TestTrue(TEXT("Secret status for a missing object is not set"), Target->GetSecretStatus(SecretKey).Source == EO3DSecretSource::None);
	TestTrue(TEXT("Rows of a missing object stay listed"), Panel->GetNumVisibleFields() == MakeSchema().Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelRegisteredTransportsTest, "Open3DBroadcast.Editor.OptionsPanel.RegisteredTransportsOpenClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelRegisteredTransportsTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelTestUtil;

	const int32 QueueBefore = GetUndoQueueLength();

	// ADR 0010 §8: opening each registered transport's panel leaves the object unchanged, and every
	// schema is well formed. Loopback is always registered; the others depend on the build flags.
	const TArray<FName> SenderTransports = FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Sender);
	TestTrue(TEXT("Loopback sender registered"), SenderTransports.Contains(FName(TEXT("Loopback"))));

	for (const FName& Transport : SenderTransports)
	{
		FO3DTransportOptionSchema Schema;
		FO3DTransportRegistry::Get().GetOptionSchema(Transport, EO3DTransportRole::Sender, Schema);
		TArray<FString> SecretKeys;
		TMap<FString, FString> SecretEnvVars;
		FO3DTransportRegistry::Get().GetSecretDeclaration(Transport, EO3DTransportRole::Sender, SecretKeys, SecretEnvVars);

		for (const FO3DTransportOptionField& Field : Schema)
		{
			const FString Where = FString::Printf(TEXT("sender %s, field '%s'"), *Transport.ToString(), *Field.Key);
			TestFalse(*(Where + TEXT(": key set")), Field.Key.IsEmpty());
			TestFalse(*(Where + TEXT(": display name set")), Field.DisplayName.IsEmpty());
			if (Field.Type == EO3DTransportOptionType::Secret)
			{
				TestTrue(*(Where + TEXT(": Secret field in the secret declaration")), SecretKeys.Contains(Field.Key));
			}
			if (Field.Type == EO3DTransportOptionType::Int)
			{
				TestTrue(*(Where + TEXT(": Int range")), Field.Max >= Field.Min);
			}
			if (Field.Type == EO3DTransportOptionType::Enum)
			{
				TestTrue(*(Where + TEXT(": Enum has values")), Field.EnumValues.Num() > 0);
			}
		}

		UO3DSenderComponent* Component = NewSender(Transport);
		TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
			.Target(MakeShared<FO3DSenderOptionTarget>(Component))
			.Schema(Schema);
		TestEqual(*FString::Printf(TEXT("sender %s: Construct leaves the options empty"), *Transport.ToString()), Component->TransportOptions.Num(), 0);
		TestTrue(*FString::Printf(TEXT("sender %s: rows within the schema"), *Transport.ToString()), Panel->GetNumVisibleFields() <= Schema.Num());
	}

	const TArray<FName> ReceiverTransports = FO3DTransportRegistry::Get().GetNames(EO3DTransportRole::Receiver);
	TestTrue(TEXT("Loopback receiver registered"), ReceiverTransports.Contains(FName(TEXT("Loopback"))));

	for (const FName& Transport : ReceiverTransports)
	{
		FO3DTransportOptionSchema Schema;
		FO3DTransportRegistry::Get().GetOptionSchema(Transport, EO3DTransportRole::Receiver, Schema);
		for (const FO3DTransportOptionField& Field : Schema)
		{
			if (Field.Type == EO3DTransportOptionType::Secret)
			{
				TestTrue(*FString::Printf(TEXT("receiver %s, field '%s': Secret field in the secret declaration"), *Transport.ToString(), *Field.Key),
					O3DReceiver::IsSecretOptionKey(Transport, Field.Key));
			}
		}

		UO3DReceiverSettingsObject* Settings = NewReceiverSettings(Transport);
		TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
			.Target(MakeShared<FO3DReceiverOptionTarget>(Settings))
			.Schema(Schema);
		TestEqual(*FString::Printf(TEXT("receiver %s: Construct leaves the options empty"), *Transport.ToString()), Settings->Settings.TransportOptions.Num(), 0);
		TestTrue(*FString::Printf(TEXT("receiver %s: rows within the schema"), *Transport.ToString()), Panel->GetNumVisibleFields() <= Schema.Num());
	}

	TestEqual(TEXT("Opening every panel records no transaction"), GetUndoQueueLength(), QueueBefore);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
