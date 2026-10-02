// Copyright Lifelike & Believable. All Rights Reserved.

// WP-A1 PR 5c (ADR 0007 item 8, ADR 0010 §4): the schema additions in the editor's options panel.
// - bRestartOnChange: the field's tooltip says so, and a commit that changes it, and only such a
//   commit, asks the target to restart its running transport.
// - Validate: a refused value is not written and its error shows under the row until a value is
//   accepted.
// - Float: parsed, clamped to Min and Max, stored as decimal text.
// Uses a counting option target over a transient sender component and test-only keys. No network.
// Needs the editor (the panel opens transactions).

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "UObject/Package.h"

#include "O3DSenderComponent.h"
#include "O3DTransportOptionSchema.h"
#include "O3DTransportOptionTarget.h"
#include "SO3DTransportOptionsPanel.h"

namespace O3DPanelSchemaTestUtil
{
	static const TCHAR* const RestartKey = TEXT("o3dpanelschema.port");
	static const TCHAR* const PlainKey = TEXT("o3dpanelschema.label");
	static const TCHAR* const CheckedKey = TEXT("o3dpanelschema.host");
	static const TCHAR* const FloatKey = TEXT("o3dpanelschema.timeout");

	static FO3DTransportOptionSchema MakeSchema()
	{
		FO3DTransportOptionSchema Schema;

		FO3DTransportOptionField Restart;
		Restart.Key = RestartKey;
		Restart.DisplayName = FText::FromString(TEXT("Port"));
		Restart.Tooltip = FText::FromString(TEXT("Port to listen on."));
		Restart.Type = EO3DTransportOptionType::Int;
		Restart.bRestartOnChange = true;
		Schema.Add(Restart);

		FO3DTransportOptionField Plain;
		Plain.Key = PlainKey;
		Plain.DisplayName = FText::FromString(TEXT("Label"));
		Plain.Tooltip = FText::FromString(TEXT("A label."));
		Plain.Type = EO3DTransportOptionType::String;
		Schema.Add(Plain);

		FO3DTransportOptionField Checked;
		Checked.Key = CheckedKey;
		Checked.DisplayName = FText::FromString(TEXT("Host"));
		Checked.Type = EO3DTransportOptionType::String;
		Checked.Validate = [](const FString& Value, FText& OutError)
		{
			if (Value.Contains(TEXT(" ")))
			{
				OutError = FText::FromString(TEXT("A host name has no spaces."));
				return false;
			}
			return true;
		};
		Schema.Add(Checked);

		FO3DTransportOptionField Timeout;
		Timeout.Key = FloatKey;
		Timeout.DisplayName = FText::FromString(TEXT("Timeout (s)"));
		Timeout.Type = EO3DTransportOptionType::Float;
		Timeout.Default = TEXT("1.5");
		Timeout.Min = 0.25;
		Timeout.Max = 30.0;
		Schema.Add(Timeout);

		return Schema;
	}

	/** Edits a transient sender component's options and counts restart requests. */
	class FCountingOptionTarget final : public IO3DOptionTarget
	{
	public:
		explicit FCountingOptionTarget(UO3DSenderComponent* InComponent)
			: WeakComponent(InComponent)
		{
		}

		virtual bool IsValid() const override { return WeakComponent.IsValid(); }
		virtual FName GetTransportName() const override { return WeakComponent.IsValid() ? WeakComponent->GetTransportName() : NAME_None; }
		virtual TMap<FString, FString> GetOptions() const override { return WeakComponent.IsValid() ? WeakComponent->TransportOptions : TMap<FString, FString>(); }
		virtual bool IsSecretKey(const FString& Key) const override { return false; }
		virtual FO3DSecretStatus GetSecretStatus(const FString& Key) const override { return FO3DSecretStatus(); }
		virtual void SetSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence) override {}
		virtual bool SetSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence) override { return false; }
		virtual void ClearSecret(const FString& Key) override {}
		virtual bool RestartTransport() override
		{
			++RestartCalls;
			return true;
		}

		int32 RestartCalls = 0;

	protected:
		virtual UObject* GetObject() const override { return WeakComponent.Get(); }
		virtual void WriteOption(const FString& Key, const FString& Value) override
		{
			if (UO3DSenderComponent* Component = WeakComponent.Get())
			{
				Component->SetTransportOption(Key, Value);
			}
		}

	private:
		TWeakObjectPtr<UO3DSenderComponent> WeakComponent;
	};

	static UO3DSenderComponent* NewSender()
	{
		UO3DSenderComponent* Component = NewObject<UO3DSenderComponent>(GetTransientPackage(), NAME_None, RF_Transactional);
		Component->TransportOptions.Reset();
		return Component;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelRestartOnChangeTest, "Open3DBroadcast.Editor.OptionsPanel.RestartOnChangeRestartsTransport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelRestartOnChangeTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelSchemaTestUtil;

	UO3DSenderComponent* Component = NewSender();
	const TSharedRef<FCountingOptionTarget> Target = MakeShared<FCountingOptionTarget>(Component);
	TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
		.Target(Target)
		.Schema(MakeSchema());

	// The metadata reaches the user: the tooltip of a restart field says so, others do not.
	const FString RestartTooltip = Panel->GetFieldTooltip(RestartKey).ToString();
	TestTrue(TEXT("Restart field keeps its tooltip"), RestartTooltip.Contains(TEXT("Port to listen on.")));
	TestTrue(TEXT("Restart field's tooltip has the note"), RestartTooltip.Contains(TEXT("restarts a running transport")));
	TestFalse(TEXT("Other fields' tooltips have no note"), Panel->GetFieldTooltip(PlainKey).ToString().Contains(TEXT("restarts")));
	TestEqual(TEXT("Constructing the panel restarts nothing"), Target->RestartCalls, 0);

	// A change to a restart field: one restart.
	TestTrue(TEXT("Port commit changes the component"), Panel->CommitFieldValue(RestartKey, TEXT("9100")));
	TestEqual(TEXT("One restart for the change"), Target->RestartCalls, 1);

	// The same value again changes nothing and restarts nothing.
	TestFalse(TEXT("Unchanged commit"), Panel->CommitFieldValue(RestartKey, TEXT("9100")));
	TestEqual(TEXT("No restart for an unchanged commit"), Target->RestartCalls, 1);

	// A field without the flag: written, no restart (the value is used at the next start, as before).
	TestTrue(TEXT("Label commit changes the component"), Panel->CommitFieldValue(PlainKey, TEXT("stage-a")));
	TestEqual(TEXT("No restart for a field without the flag"), Target->RestartCalls, 1);

	// The real sender target restarts only a component that is capturing in a game world.
	FO3DSenderOptionTarget SenderTarget(Component);
	TestFalse(TEXT("An idle component is not restarted"), SenderTarget.RestartTransport());
	TestFalse(TEXT("It was not started by the request"), Component->IsCapturing());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelValidateTest, "Open3DBroadcast.Editor.OptionsPanel.ValidateShowsErrorAndRefuses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelValidateTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelSchemaTestUtil;

	UO3DSenderComponent* Component = NewSender();
	TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FCountingOptionTarget>(Component))
		.Schema(MakeSchema());

	TestTrue(TEXT("No error before a commit"), Panel->GetFieldError(CheckedKey).IsEmpty());

	TestFalse(TEXT("A refused value is not committed"), Panel->CommitFieldValue(CheckedKey, TEXT("mocap pc")));
	TestFalse(TEXT("Nothing was written"), Component->TransportOptions.Contains(CheckedKey));
	TestEqual(TEXT("The error shows under the row"), Panel->GetFieldError(CheckedKey).ToString(), FString(TEXT("A host name has no spaces.")));

	TestTrue(TEXT("An accepted value is committed"), Panel->CommitFieldValue(CheckedKey, TEXT("mocap-pc")));
	TestEqual(TEXT("It was written"), Component->GetTransportOption(CheckedKey), FString(TEXT("mocap-pc")));
	TestTrue(TEXT("The error is gone"), Panel->GetFieldError(CheckedKey).IsEmpty());

	// Clearing the box resets to the default and is never refused.
	TestTrue(TEXT("Clearing is accepted"), Panel->CommitFieldValue(CheckedKey, TEXT("")));
	TestFalse(TEXT("The key is removed"), Component->TransportOptions.Contains(CheckedKey));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FO3DOptionsPanelFloatTest, "Open3DBroadcast.Editor.OptionsPanel.FloatIsClampedToRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FO3DOptionsPanelFloatTest::RunTest(const FString& Parameters)
{
	using namespace O3DPanelSchemaTestUtil;

	UO3DSenderComponent* Component = NewSender();
	TSharedRef<SO3DTransportOptionsPanel> Panel = SNew(SO3DTransportOptionsPanel)
		.Target(MakeShared<FCountingOptionTarget>(Component))
		.Schema(MakeSchema());

	TestEqual(TEXT("Construct writes no default"), Component->TransportOptions.Num(), 0);

	TestTrue(TEXT("A value in range"), Panel->CommitFieldValue(FloatKey, TEXT(" 2.5 ")));
	TestEqual(TEXT("Stored as decimal text"), Component->GetTransportOption(FloatKey), FString(TEXT("2.5")));

	Panel->CommitFieldValue(FloatKey, TEXT("120"));
	TestEqual(TEXT("Clamped to Max"), Component->GetTransportOption(FloatKey), FString(TEXT("30.0")));

	Panel->CommitFieldValue(FloatKey, TEXT("0.01"));
	TestEqual(TEXT("Clamped to Min"), Component->GetTransportOption(FloatKey), FString(TEXT("0.25")));

	TestFalse(TEXT("Not a number is refused"), Panel->CommitFieldValue(FloatKey, TEXT("soon")));
	TestEqual(TEXT("The stored value is kept"), Component->GetTransportOption(FloatKey), FString(TEXT("0.25")));

	TestTrue(TEXT("An empty box resets to the default"), Panel->CommitFieldValue(FloatKey, TEXT("")));
	TestFalse(TEXT("The key is removed"), Component->TransportOptions.Contains(FloatKey));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
