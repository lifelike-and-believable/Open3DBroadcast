// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class IO3DOptionTarget;
class SEditableTextBox;

DECLARE_DELEGATE_OneParam(FO3DOnSecretCommitted, ETextCommit::Type);

/**
 * One secret option (ADR 0004 item 5), generalised from the WebRTC panel's secret field: a password
 * box that always opens empty, a Clear button, a "Remember on this machine" checkbox and a status
 * line saying where the current value comes from. Committing an empty box keeps the current value;
 * Clear removes it. Writes go to the target's secret store, never to the option map, and open no
 * transaction.
 */
class SO3DSecretOptionField : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SO3DSecretOptionField) {}
		SLATE_ARGUMENT(TSharedPtr<IO3DOptionTarget>, Target)
		SLATE_ARGUMENT(FString, Key)
		SLATE_ARGUMENT(FText, HintText)
		SLATE_EVENT(FO3DOnSecretCommitted, OnCommitted)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	void HandleCommitted(const FText& NewText, ETextCommit::Type CommitType);
	FReply HandleClearClicked();
	ECheckBoxState GetRememberState() const;
	void HandleRememberChanged(ECheckBoxState NewState);
	FText GetStatusText() const;

	TSharedPtr<IO3DOptionTarget> Target;
	FString Key;
	FO3DOnSecretCommitted OnCommitted;
	TSharedPtr<SEditableTextBox> ValueTextBox;
	bool bRemember = false;
};
