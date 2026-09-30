// Copyright Lifelike & Believable. All Rights Reserved.

#include "SO3DSecretOptionField.h"

#include "O3DSecretStore.h"
#include "O3DTransportOptionTarget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "O3DSecretOptionField"

void SO3DSecretOptionField::Construct(const FArguments& InArgs)
{
	Target = InArgs._Target;
	Key = InArgs._Key;
	OnCommitted = InArgs._OnCommitted;
	bRemember = Target.IsValid() && Target->GetSecretStatus(Key).bRemembered;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 0.f, 0.f, 2.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.f)
			[
				SAssignNew(ValueTextBox, SEditableTextBox)
				.Text(FText::GetEmpty())
				.IsPassword(true)
				.HintText(InArgs._HintText)
				.OnTextCommitted(this, &SO3DSecretOptionField::HandleCommitted)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4.f, 0.f, 0.f, 0.f)
			[
				SNew(SButton)
				.Text(LOCTEXT("SecretClear", "Clear"))
				.ToolTipText(LOCTEXT("SecretClearTooltip", "Forget the value for this session and on this machine. An environment variable still applies."))
				.OnClicked(this, &SO3DSecretOptionField::HandleClearClicked)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.f, 0.f, 0.f, 2.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SCheckBox)
				.IsChecked(this, &SO3DSecretOptionField::GetRememberState)
				.OnCheckStateChanged(this, &SO3DSecretOptionField::HandleRememberChanged)
				.ToolTipText(LOCTEXT("SecretRememberTooltip", "Keep the value in your per-user editor settings under Saved/Config on this machine. It is never written to the level, the Blueprint, the project ini or a LiveLink preset."))
			]
			+ SHorizontalBox::Slot()
			.Padding(4.f, 0.f, 0.f, 0.f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("SecretRememberLabel", "Remember on this machine"))
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(this, &SO3DSecretOptionField::GetStatusText)
		]
	];
}

void SO3DSecretOptionField::HandleCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	const FString Value = NewText.ToString().TrimStartAndEnd();
	if (Value.IsEmpty() || CommitType == ETextCommit::OnCleared)
	{
		// The box opens empty; leaving it empty keeps the current value. Clear removes it.
		return;
	}

	if (Target.IsValid())
	{
		Target->SetSecret(Key, Value, bRemember ? EO3DSecretPersistence::RememberOnThisMachine : EO3DSecretPersistence::Session);
	}
	if (ValueTextBox.IsValid())
	{
		ValueTextBox->SetText(FText::GetEmpty());
	}
	OnCommitted.ExecuteIfBound(CommitType);
}

FReply SO3DSecretOptionField::HandleClearClicked()
{
	if (Target.IsValid())
	{
		Target->ClearSecret(Key);
	}
	bRemember = false;
	if (ValueTextBox.IsValid())
	{
		ValueTextBox->SetText(FText::GetEmpty());
	}
	OnCommitted.ExecuteIfBound(ETextCommit::Default);
	return FReply::Handled();
}

ECheckBoxState SO3DSecretOptionField::GetRememberState() const
{
	return bRemember ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SO3DSecretOptionField::HandleRememberChanged(ECheckBoxState NewState)
{
	bRemember = (NewState == ECheckBoxState::Checked);
	// Moves a value set this session; unchecking also forgets a remembered copy.
	if (Target.IsValid())
	{
		Target->SetSecretPersistence(Key, bRemember ? EO3DSecretPersistence::RememberOnThisMachine : EO3DSecretPersistence::Session);
	}
}

FText SO3DSecretOptionField::GetStatusText() const
{
	const FO3DSecretStatus Status = Target.IsValid() ? Target->GetSecretStatus(Key) : FO3DSecretStatus();
	switch (Status.Source)
	{
	case EO3DSecretSource::Session:
		return Status.bRemembered
			? LOCTEXT("SecretStatusSessionRemembered", "Set for this session and remembered on this machine")
			: LOCTEXT("SecretStatusSession", "Set for this session (gone after restart)");
	case EO3DSecretSource::Environment:
		return FText::Format(LOCTEXT("SecretStatusEnv", "From environment variable {0}"), FText::FromString(Status.EnvVarName));
	case EO3DSecretSource::UserSettings:
		return LOCTEXT("SecretStatusRemembered", "Remembered on this machine");
	default:
		return LOCTEXT("SecretStatusNotSet", "Not set");
	}
}

#undef LOCTEXT_NAMESPACE
