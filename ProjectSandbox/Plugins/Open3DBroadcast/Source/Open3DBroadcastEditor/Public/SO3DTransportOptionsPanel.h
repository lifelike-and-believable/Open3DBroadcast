// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Layout/Visibility.h"
#include "O3DTransportConfigPanelBase.h"
#include "O3DTransportOptionSchema.h"
#include "Styling/SlateTypes.h"
#include "Types/SlateEnums.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class IO3DOptionTarget;
class SWidget;

/**
 * One panel for every transport (ADR 0010 §4 and §5). It renders a transport's
 * FO3DTransportOptionSchema against an IO3DOptionTarget and is used both in the sender Details
 * panel and in the receiver's LiveLink "Add Source" panel.
 *
 * Behaviour (TRB-45, SND-35):
 * - Construct() reads the target and never writes to it. A default is shown as a hint until the
 *   user commits a value.
 * - Text boxes and number boxes write only when a value is committed (Enter, focus change, or the
 *   end of a spin-box drag). Dragging a number writes once, on release.
 * - Each commit is one undoable transaction (IO3DOptionTarget::CommitOption).
 * - Secret fields write to the secret store only, never to the option map or the undo buffer.
 * - The target is held weakly; once it is gone the rows are disabled and a line says so.
 */
class OPEN3DBROADCASTEDITOR_API SO3DTransportOptionsPanel : public SO3DTransportConfigPanelBase
{
public:
	SLATE_BEGIN_ARGS(SO3DTransportOptionsPanel)
		: _PanelWidth(SO3DTransportConfigPanelBase::DefaultPanelWidth)
	{}
		/** The object to edit. Required. */
		SLATE_ARGUMENT(TSharedPtr<IO3DOptionTarget>, Target)
		/** The transport's declared options, in display order. */
		SLATE_ARGUMENT(FO3DTransportOptionSchema, Schema)
		SLATE_ARGUMENT(float, PanelWidth)
		/** Enter in a text or number box (the LiveLink panel creates the source). */
		SLATE_EVENT(FSimpleDelegate, OnSubmit)
		/** After each commit that changed the target. */
		SLATE_EVENT(FSimpleDelegate, OnOptionCommitted)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/**
	 * Commits Value for the field Key the way its widget does: text is trimmed, an Int is parsed,
	 * clamped to the field's range and scaled to the stored unit, an Enum must be one of its
	 * values. One transaction when the stored value changes. Returns true when it changed.
	 * Public so tests can drive the panel without synthesizing Slate input.
	 */
	bool CommitFieldValue(const FString& Key, const FString& Value);

	/** Stores a secret for the Secret field Key, as its password box does. Never enters the option map. */
	void CommitSecretValue(const FString& Key, const FString& Value);

	/** Number of rows currently visible (fields whose VisibleWhen passes). For tests. */
	int32 GetNumVisibleFields() const;

private:
	/** Choices of one Enum field, owned here because SComboBox keeps a pointer to its source array. */
	struct FEnumChoices
	{
		TArray<TSharedPtr<FO3DTransportOptionEnumValue>> Items;
	};

	TSharedRef<SWidget> BuildFieldWidget(int32 FieldIndex);
	TSharedRef<SWidget> BuildEnumWidget(int32 FieldIndex);

	const FO3DTransportOptionField* FindField(const FString& Key, int32* OutIndex = nullptr) const;
	bool IsFieldVisible(int32 FieldIndex) const;
	EVisibility GetFieldVisibility(int32 FieldIndex) const;
	EVisibility GetInvalidTargetVisibility() const;
	bool IsTargetValid() const;

	/** Stored value, empty when unset. */
	FString GetStoredValue(int32 FieldIndex) const;
	/** Text shown in an empty box: the field's Hint, else its Default (converted to the shown unit). */
	FText GetHintText(int32 FieldIndex) const;
	/** Converts the stored Int text to the shown unit; unset when empty or not a number. */
	static TOptional<int32> StoredToShown(const FO3DTransportOptionField& Field, const FString& Stored);

	FText GetTextValue(int32 FieldIndex) const;
	TOptional<int32> GetIntValue(int32 FieldIndex) const;
	ECheckBoxState GetBoolValue(int32 FieldIndex) const;
	FText GetEnumText(int32 FieldIndex) const;

	void HandleTextCommitted(const FText& NewText, ETextCommit::Type CommitType, int32 FieldIndex);
	void HandleIntCommitted(int32 NewValue, ETextCommit::Type CommitType, int32 FieldIndex);
	void HandleBoolChanged(ECheckBoxState NewState, int32 FieldIndex);
	void HandleEnumChanged(TSharedPtr<FO3DTransportOptionEnumValue> NewSelection, ESelectInfo::Type SelectInfo, int32 FieldIndex);
	void HandleSecretCommitted(ETextCommit::Type CommitType);

	/** CommitFieldValue for a known field. */
	bool CommitFieldAt(int32 FieldIndex, const FString& Value);
	/** Writes one stored value through the target; notifies OnOptionCommitted when it changed. */
	bool CommitStoredValue(int32 FieldIndex, const FString& StoredValue);

	TSharedPtr<IO3DOptionTarget> Target;
	FO3DTransportOptionSchema Schema;
	TArray<FEnumChoices> EnumChoices;
	FSimpleDelegate OnOptionCommitted;
};
