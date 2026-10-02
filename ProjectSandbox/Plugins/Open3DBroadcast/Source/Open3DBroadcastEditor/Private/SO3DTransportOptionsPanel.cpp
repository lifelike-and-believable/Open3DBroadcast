// Copyright Lifelike & Believable. All Rights Reserved.

#include "SO3DTransportOptionsPanel.h"

#include "Math/UnrealMathUtility.h"
#include "Framework/SlateDelegates.h"
#include "Misc/Attribute.h"
#include "O3DSecretStore.h"
#include "O3DTransportOptionTarget.h"
#include "SO3DSecretOptionField.h"
#include "Transport/O3DTransportOptions.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "O3DTransportOptionsPanel"

namespace O3DTransportOptionsPanelPrivate
{
	/** The whole-number bounds of an Int field's range (Min and Max are doubles since WP-A1 PR 5c). */
	int32 IntRangeLow(const FO3DTransportOptionField& Field)
	{
		return static_cast<int32>(FMath::Clamp(FMath::CeilToDouble(Field.Min), static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)));
	}

	int32 IntRangeHigh(const FO3DTransportOptionField& Field)
	{
		return static_cast<int32>(FMath::Clamp(FMath::FloorToDouble(Field.Max), static_cast<double>(MIN_int32), static_cast<double>(MAX_int32)));
	}
}

void SO3DTransportOptionsPanel::Construct(const FArguments& InArgs)
{
	Target = InArgs._Target;
	Schema = InArgs._Schema;
	OnOptionCommitted = InArgs._OnOptionCommitted;
	SetOnSubmit(InArgs._OnSubmit);

	// Sized once: each SComboBox keeps a pointer to its entry's Items array.
	EnumChoices.SetNum(Schema.Num());
	FieldErrors.SetNum(Schema.Num());
	for (int32 FieldIndex = 0; FieldIndex < Schema.Num(); ++FieldIndex)
	{
		if (Schema[FieldIndex].Type == EO3DTransportOptionType::Enum)
		{
			for (const FO3DTransportOptionEnumValue& Choice : Schema[FieldIndex].EnumValues)
			{
				EnumChoices[FieldIndex].Items.Add(MakeShared<FO3DTransportOptionEnumValue>(Choice));
			}
		}
	}

	// Construct() only reads the target. Nothing below writes to it (TRB-45).
	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);

	Rows->AddSlot()
		.AutoHeight()
		.Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("TargetGone", "The object these options belong to no longer exists. Select it again to edit its transport options."))
			.AutoWrapText(true)
			.Visibility(TAttribute<EVisibility>::CreateSP(this, &SO3DTransportOptionsPanel::GetInvalidTargetVisibility))
		];

	for (int32 FieldIndex = 0; FieldIndex < Schema.Num(); ++FieldIndex)
	{
		const FO3DTransportOptionField& Field = Schema[FieldIndex];

		Rows->AddSlot()
			.AutoHeight()
			.Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(SVerticalBox)
				.Visibility(TAttribute<EVisibility>::CreateSP(this, &SO3DTransportOptionsPanel::GetFieldVisibility, FieldIndex))
				.IsEnabled(TAttribute<bool>::CreateSP(this, &SO3DTransportOptionsPanel::IsTargetValid))
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Field.DisplayName)
					.ToolTipText(GetFieldTooltipAt(FieldIndex))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 4.f, 0.f, 0.f)
				[
					BuildFieldWidget(FieldIndex)
				]
				// Why the last value was refused (the field's Validate, WP-A1 PR 5c).
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(STextBlock)
					.Text(TAttribute<FText>::CreateSP(this, &SO3DTransportOptionsPanel::GetFieldErrorText, FieldIndex))
					.ColorAndOpacity(FLinearColor(1.f, 0.4f, 0.3f))
					.AutoWrapText(true)
					.Visibility(TAttribute<EVisibility>::CreateSP(this, &SO3DTransportOptionsPanel::GetFieldErrorVisibility, FieldIndex))
				]
			];
	}

	BuildPanel(Rows, InArgs._PanelWidth);
}

TSharedRef<SWidget> SO3DTransportOptionsPanel::BuildFieldWidget(int32 FieldIndex)
{
	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	const FText Tooltip = GetFieldTooltipAt(FieldIndex);

	switch (Field.Type)
	{
	case EO3DTransportOptionType::Int:
	{
		const bool bHasRange = Field.Max > Field.Min;
		const TOptional<int32> MinValue = bHasRange ? TOptional<int32>(O3DTransportOptionsPanelPrivate::IntRangeLow(Field)) : TOptional<int32>();
		const TOptional<int32> MaxValue = bHasRange ? TOptional<int32>(O3DTransportOptionsPanelPrivate::IntRangeHigh(Field)) : TOptional<int32>();

		// Writes on commit only: Enter, focus change, or the end of a spin drag (TRB-45). No
		// OnValueChanged binding, so dragging does not write or restart anything per tick.
		return SNew(SNumericEntryBox<int32>)
			.AllowSpin(bHasRange)
			.MinValue(MinValue)
			.MaxValue(MaxValue)
			.MinSliderValue(MinValue)
			.MaxSliderValue(MaxValue)
			.Value(TAttribute<TOptional<int32>>::CreateSP(this, &SO3DTransportOptionsPanel::GetIntValue, FieldIndex))
			.UndeterminedString(GetHintText(FieldIndex))
			.OnValueCommitted(this, &SO3DTransportOptionsPanel::HandleIntCommitted, FieldIndex)
			.ToolTipText(Tooltip);
	}

	case EO3DTransportOptionType::Float:
	{
		const bool bHasRange = Field.Max > Field.Min;
		const TOptional<double> MinValue = bHasRange ? TOptional<double>(Field.Min) : TOptional<double>();
		const TOptional<double> MaxValue = bHasRange ? TOptional<double>(Field.Max) : TOptional<double>();

		// Like the Int box: writes on commit only (TRB-45).
		return SNew(SNumericEntryBox<double>)
			.AllowSpin(bHasRange)
			.MinValue(MinValue)
			.MaxValue(MaxValue)
			.MinSliderValue(MinValue)
			.MaxSliderValue(MaxValue)
			.Value(TAttribute<TOptional<double>>::CreateSP(this, &SO3DTransportOptionsPanel::GetFloatValue, FieldIndex))
			.UndeterminedString(GetHintText(FieldIndex))
			.OnValueCommitted(this, &SO3DTransportOptionsPanel::HandleFloatCommitted, FieldIndex)
			.ToolTipText(Tooltip);
	}

	case EO3DTransportOptionType::Bool:
		return SNew(SCheckBox)
			.IsChecked(TAttribute<ECheckBoxState>::CreateSP(this, &SO3DTransportOptionsPanel::GetBoolValue, FieldIndex))
			.OnCheckStateChanged(FOnCheckStateChanged::CreateSP(this, &SO3DTransportOptionsPanel::HandleBoolChanged, FieldIndex))
			.ToolTipText(Tooltip);

	case EO3DTransportOptionType::Enum:
		return BuildEnumWidget(FieldIndex);

	case EO3DTransportOptionType::Secret:
		return SNew(SO3DSecretOptionField)
			.Target(Target)
			.Key(Field.Key)
			.HintText(GetHintText(FieldIndex))
			.OnCommitted(FO3DOnSecretCommitted::CreateSP(this, &SO3DTransportOptionsPanel::HandleSecretCommitted));

	case EO3DTransportOptionType::String:
	case EO3DTransportOptionType::Url:
	default:
		return SNew(SEditableTextBox)
			.Text(TAttribute<FText>::CreateSP(this, &SO3DTransportOptionsPanel::GetTextValue, FieldIndex))
			.HintText(GetHintText(FieldIndex))
			.OnTextCommitted(FOnTextCommitted::CreateSP(this, &SO3DTransportOptionsPanel::HandleTextCommitted, FieldIndex))
			.ToolTipText(Tooltip);
	}
}

TSharedRef<SWidget> SO3DTransportOptionsPanel::BuildEnumWidget(int32 FieldIndex)
{
	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	TArray<TSharedPtr<FO3DTransportOptionEnumValue>>& Items = EnumChoices[FieldIndex].Items;

	FString Current = GetStoredValue(FieldIndex);
	if (Current.IsEmpty())
	{
		Current = Field.Default;
	}

	TSharedPtr<FO3DTransportOptionEnumValue> Initial;
	for (const TSharedPtr<FO3DTransportOptionEnumValue>& Item : Items)
	{
		if (Item.IsValid() && Item->Value.Equals(Current, ESearchCase::IgnoreCase))
		{
			Initial = Item;
			break;
		}
	}

	using FEnumCombo = SComboBox<TSharedPtr<FO3DTransportOptionEnumValue>>;
	return SNew(FEnumCombo)
		.OptionsSource(&Items)
		.InitiallySelectedItem(Initial)
		.OnGenerateWidget_Lambda([](TSharedPtr<FO3DTransportOptionEnumValue> Item)
		{
			return SNew(STextBlock).Text(Item.IsValid() ? Item->DisplayName : FText::GetEmpty());
		})
		.OnSelectionChanged(FEnumCombo::FOnSelectionChanged::CreateSP(this, &SO3DTransportOptionsPanel::HandleEnumChanged, FieldIndex))
		.ToolTipText(GetFieldTooltipAt(FieldIndex))
		[
			SNew(STextBlock)
			.Text(TAttribute<FText>::CreateSP(this, &SO3DTransportOptionsPanel::GetEnumText, FieldIndex))
		];
}

bool SO3DTransportOptionsPanel::CommitFieldValue(const FString& Key, const FString& Value)
{
	int32 FieldIndex = INDEX_NONE;
	return FindField(Key, &FieldIndex) ? CommitFieldAt(FieldIndex, Value) : false;
}

void SO3DTransportOptionsPanel::CommitSecretValue(const FString& Key, const FString& Value)
{
	const FO3DTransportOptionField* Field = FindField(Key);
	if (!Field || Field->Type != EO3DTransportOptionType::Secret || !Target.IsValid())
	{
		return;
	}

	const FString Trimmed = Value.TrimStartAndEnd();
	if (!Trimmed.IsEmpty())
	{
		Target->SetSecret(Key, Trimmed, EO3DSecretPersistence::Session);
	}
}

FText SO3DTransportOptionsPanel::GetFieldError(const FString& Key) const
{
	int32 FieldIndex = INDEX_NONE;
	return FindField(Key, &FieldIndex) ? GetFieldErrorText(FieldIndex) : FText::GetEmpty();
}

FText SO3DTransportOptionsPanel::GetFieldTooltip(const FString& Key) const
{
	int32 FieldIndex = INDEX_NONE;
	return FindField(Key, &FieldIndex) ? GetFieldTooltipAt(FieldIndex) : FText::GetEmpty();
}

FText SO3DTransportOptionsPanel::GetFieldTooltipAt(int32 FieldIndex) const
{
	if (!Schema.IsValidIndex(FieldIndex))
	{
		return FText::GetEmpty();
	}

	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	if (!Field.bRestartOnChange)
	{
		return Field.Tooltip;
	}
	const FText RestartNote = LOCTEXT("RestartOnChangeNote", "Changing this restarts a running transport.");
	return Field.Tooltip.IsEmpty()
		? RestartNote
		: FText::Format(LOCTEXT("TooltipWithRestartNote", "{0}\n\n{1}"), Field.Tooltip, RestartNote);
}

FText SO3DTransportOptionsPanel::GetFieldErrorText(int32 FieldIndex) const
{
	return FieldErrors.IsValidIndex(FieldIndex) ? FieldErrors[FieldIndex] : FText::GetEmpty();
}

EVisibility SO3DTransportOptionsPanel::GetFieldErrorVisibility(int32 FieldIndex) const
{
	return GetFieldErrorText(FieldIndex).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

int32 SO3DTransportOptionsPanel::GetNumVisibleFields() const
{
	int32 Count = 0;
	for (int32 FieldIndex = 0; FieldIndex < Schema.Num(); ++FieldIndex)
	{
		Count += IsFieldVisible(FieldIndex) ? 1 : 0;
	}
	return Count;
}

const FO3DTransportOptionField* SO3DTransportOptionsPanel::FindField(const FString& Key, int32* OutIndex) const
{
	for (int32 FieldIndex = 0; FieldIndex < Schema.Num(); ++FieldIndex)
	{
		if (Schema[FieldIndex].Key.Equals(Key, ESearchCase::CaseSensitive))
		{
			if (OutIndex)
			{
				*OutIndex = FieldIndex;
			}
			return &Schema[FieldIndex];
		}
	}
	return nullptr;
}

bool SO3DTransportOptionsPanel::IsTargetValid() const
{
	return Target.IsValid() && Target->IsValid();
}

bool SO3DTransportOptionsPanel::IsFieldVisible(int32 FieldIndex) const
{
	if (!Schema.IsValidIndex(FieldIndex))
	{
		return false;
	}

	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	if (!Field.VisibleWhen || !IsTargetValid())
	{
		return true;
	}
	return Field.VisibleWhen(Target->GetOptions());
}

EVisibility SO3DTransportOptionsPanel::GetFieldVisibility(int32 FieldIndex) const
{
	return IsFieldVisible(FieldIndex) ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SO3DTransportOptionsPanel::GetInvalidTargetVisibility() const
{
	return IsTargetValid() ? EVisibility::Collapsed : EVisibility::Visible;
}

FString SO3DTransportOptionsPanel::GetStoredValue(int32 FieldIndex) const
{
	return (IsTargetValid() && Schema.IsValidIndex(FieldIndex)) ? Target->GetOption(Schema[FieldIndex].Key) : FString();
}

TOptional<int32> SO3DTransportOptionsPanel::StoredToShown(const FO3DTransportOptionField& Field, const FString& Stored)
{
	const FString Trimmed = Stored.TrimStartAndEnd();
	if (Trimmed.IsEmpty() || !Trimmed.IsNumeric())
	{
		return TOptional<int32>();
	}

	const int64 Scale = FMath::Max<int64>(1, Field.StoredUnitScale);
	const int64 StoredValue = FCString::Atoi64(*Trimmed);
	// Round to nearest, so a stored byte count that is not a whole MiB does not always read low.
	const int64 Shown = (StoredValue >= 0) ? (StoredValue + Scale / 2) / Scale : StoredValue / Scale;
	return static_cast<int32>(FMath::Clamp<int64>(Shown, MIN_int32, MAX_int32));
}

FText SO3DTransportOptionsPanel::GetHintText(int32 FieldIndex) const
{
	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	if (!Field.Hint.IsEmpty())
	{
		return Field.Hint;
	}
	if (Field.Default.IsEmpty())
	{
		return FText::GetEmpty();
	}

	// Not a bare number: SNumericEntryBox shows this as text while the key is unset, and a
	// numeric string would be written back when the box merely loses focus.
	FString ShownDefault = Field.Default;
	if (Field.Type == EO3DTransportOptionType::Int)
	{
		const TOptional<int32> Shown = StoredToShown(Field, Field.Default);
		if (Shown.IsSet())
		{
			ShownDefault = FString::FromInt(Shown.GetValue());
		}
	}
	return FText::Format(LOCTEXT("DefaultHint", "default: {0}"), FText::FromString(ShownDefault));
}

FText SO3DTransportOptionsPanel::GetTextValue(int32 FieldIndex) const
{
	return FText::FromString(GetStoredValue(FieldIndex));
}

TOptional<int32> SO3DTransportOptionsPanel::GetIntValue(int32 FieldIndex) const
{
	return Schema.IsValidIndex(FieldIndex) ? StoredToShown(Schema[FieldIndex], GetStoredValue(FieldIndex)) : TOptional<int32>();
}

TOptional<double> SO3DTransportOptionsPanel::GetFloatValue(int32 FieldIndex) const
{
	// Unset or not a number: no value, so the box shows the hint (as the Int box does).
	double Value = 0.0;
	return O3DTransportOptions::TryParseDouble(GetStoredValue(FieldIndex), Value) ? TOptional<double>(Value) : TOptional<double>();
}

ECheckBoxState SO3DTransportOptionsPanel::GetBoolValue(int32 FieldIndex) const
{
	FString Value = GetStoredValue(FieldIndex);
	if (Value.IsEmpty() && Schema.IsValidIndex(FieldIndex))
	{
		Value = Schema[FieldIndex].Default;
	}
	return Value.ToBool() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

FText SO3DTransportOptionsPanel::GetEnumText(int32 FieldIndex) const
{
	if (!Schema.IsValidIndex(FieldIndex))
	{
		return FText::GetEmpty();
	}

	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	FString Value = GetStoredValue(FieldIndex);
	if (Value.IsEmpty())
	{
		Value = Field.Default;
	}

	for (const FO3DTransportOptionEnumValue& Choice : Field.EnumValues)
	{
		if (Choice.Value.Equals(Value, ESearchCase::IgnoreCase))
		{
			return Choice.DisplayName;
		}
	}
	return FText::FromString(Value);
}

void SO3DTransportOptionsPanel::HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 FieldIndex)
{
	if (CommitType == ETextCommit::OnCleared)
	{
		// Escape: keep the stored value; the bound text shows it again.
		return;
	}

	CommitFieldAt(FieldIndex, NewText.ToString());
	SubmitFromTextCommit(CommitType);
}

void SO3DTransportOptionsPanel::HandleIntCommitted(int32 NewValue, ETextCommit::Type CommitType, int32 FieldIndex)
{
	if (CommitType == ETextCommit::OnCleared)
	{
		return;
	}

	CommitFieldAt(FieldIndex, FString::FromInt(NewValue));
	SubmitFromTextCommit(CommitType);
}

void SO3DTransportOptionsPanel::HandleFloatCommitted(double NewValue, ETextCommit::Type CommitType, int32 FieldIndex)
{
	if (CommitType == ETextCommit::OnCleared)
	{
		return;
	}

	CommitFieldAt(FieldIndex, FString::SanitizeFloat(NewValue));
	SubmitFromTextCommit(CommitType);
}

void SO3DTransportOptionsPanel::HandleBoolChanged(ECheckBoxState NewState, int32 FieldIndex)
{
	CommitFieldAt(FieldIndex, NewState == ECheckBoxState::Checked ? TEXT("true") : TEXT("false"));
}

void SO3DTransportOptionsPanel::HandleEnumChanged(TSharedPtr<FO3DTransportOptionEnumValue> NewSelection, ESelectInfo::Type SelectInfo, int32 FieldIndex)
{
	// Direct is a programmatic selection, not the user's.
	if (SelectInfo == ESelectInfo::Direct || !NewSelection.IsValid())
	{
		return;
	}

	CommitFieldAt(FieldIndex, NewSelection->Value);
}

void SO3DTransportOptionsPanel::HandleSecretCommitted(ETextCommit::Type CommitType)
{
	SubmitFromTextCommit(CommitType);
}

bool SO3DTransportOptionsPanel::CommitFieldAt(int32 FieldIndex, const FString& Value)
{
	if (!Schema.IsValidIndex(FieldIndex))
	{
		return false;
	}

	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	const FString Trimmed = Value.TrimStartAndEnd();

	switch (Field.Type)
	{
	case EO3DTransportOptionType::Int:
	{
		if (Trimmed.IsEmpty())
		{
			// An empty box resets the option to the transport's default.
			return CommitStoredValue(FieldIndex, FString());
		}
		if (!Trimmed.IsNumeric())
		{
			return false;
		}

		int64 Shown = FCString::Atoi64(*Trimmed);
		if (Field.Max > Field.Min)
		{
			Shown = FMath::Clamp<int64>(Shown, O3DTransportOptionsPanelPrivate::IntRangeLow(Field), O3DTransportOptionsPanelPrivate::IntRangeHigh(Field));
		}
		const int64 Stored = Shown * FMath::Max<int64>(1, Field.StoredUnitScale);
		return CommitStoredValue(FieldIndex, LexToString(Stored));
	}

	case EO3DTransportOptionType::Float:
	{
		if (Trimmed.IsEmpty())
		{
			// An empty box resets the option to the transport's default.
			return CommitStoredValue(FieldIndex, FString());
		}
		double Parsed = 0.0;
		if (!O3DTransportOptions::TryParseDouble(Trimmed, Parsed))
		{
			return false;
		}
		if (Field.Max > Field.Min)
		{
			Parsed = FMath::Clamp(Parsed, Field.Min, Field.Max);
		}
		return CommitStoredValue(FieldIndex, FString::SanitizeFloat(Parsed));
	}

	case EO3DTransportOptionType::Bool:
		return CommitStoredValue(FieldIndex, Trimmed.ToBool() ? TEXT("true") : TEXT("false"));

	case EO3DTransportOptionType::Enum:
		for (const FO3DTransportOptionEnumValue& Choice : Field.EnumValues)
		{
			if (Choice.Value.Equals(Trimmed, ESearchCase::IgnoreCase))
			{
				return CommitStoredValue(FieldIndex, Choice.Value);
			}
		}
		return false;

	case EO3DTransportOptionType::Secret:
		// Secrets never go through the option map (ADR 0004); see CommitSecretValue.
		return false;

	case EO3DTransportOptionType::String:
	case EO3DTransportOptionType::Url:
	default:
		return CommitStoredValue(FieldIndex, Trimmed);
	}
}

bool SO3DTransportOptionsPanel::CommitStoredValue(int32 FieldIndex, const FString& StoredValue)
{
	if (!IsTargetValid() || !Schema.IsValidIndex(FieldIndex))
	{
		return false;
	}

	// The field's Validate decides before anything is written (WP-A1 PR 5c). An empty value
	// resets to the default and is never refused.
	const FO3DTransportOptionField& Field = Schema[FieldIndex];
	FText Error;
	if (!O3DTransportOptions::ValidateOptionValue(Field, StoredValue, Error))
	{
		FieldErrors[FieldIndex] = Error;
		return false;
	}
	FieldErrors[FieldIndex] = FText::GetEmpty();

	const bool bChanged = Target->CommitOption(Field.Key, StoredValue);
	if (bChanged)
	{
		OnOptionCommitted.ExecuteIfBound();
		if (Field.bRestartOnChange)
		{
			Target->RestartTransport();
		}
	}
	return bChanged;
}

#undef LOCTEXT_NAMESPACE
