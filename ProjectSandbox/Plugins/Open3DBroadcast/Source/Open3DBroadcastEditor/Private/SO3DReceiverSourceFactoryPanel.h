// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LiveLinkSourceFactory.h"
#include "O3DReceiverSourceSettings.h"
#include "Types/SlateEnums.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SCompoundWidget.h"

class IDetailsView;
class SBox;
struct FPropertyAndParent;
struct FPropertyChangedEvent;

/**
 * The LiveLink "Add Source" panel of UO3DReceiverSourceFactory (ADR 0010 §3). Moved from
 * Open3DReceiver/Private/O3DReceiverSourceFactory.cpp; the factory reaches it through
 * O3DReceiver::SetSourceFactoryPanelBuilder, which the editor module sets at startup.
 *
 * The panel edits its own transient copy of the receiver defaults, so nothing is saved until the
 * user presses Create Source. Transport and codec choices are undoable edits of that copy; the
 * transport options come from the transport's schema through SO3DTransportOptionsPanel.
 */
class SO3DReceiverSourceFactoryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SO3DReceiverSourceFactoryPanel) {}
		SLATE_ARGUMENT(ULiveLinkSourceFactory::FOnLiveLinkSourceCreated, OnSourceCreated)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

	FReply OnCreateClicked();

	void HandleSettingsPropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent);
	bool HandleIsPropertyVisible(const FPropertyAndParent& PropertyAndParent) const;

	void RefreshTransportCustomization();
	/** Enter in the transport options panel creates the source, as it did in the per-transport panels. */
	void HandleOptionsSubmit();
	FName GetCurrentTransportName() const;

	void RefreshAudioCodecOptions();
	void SyncAudioCodecSelection();
	void HandleAudioCodecSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo);
	TSharedRef<SWidget> GenerateAudioCodecWidget(TSharedPtr<FName> InItem) const;
	FText GetSelectedAudioCodecText() const;
	bool IsAudioCodecSelectionEnabled() const;

	void RefreshTransportOptions();
	void SyncTransportSelection();
	void HandleTransportSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo);
	TSharedRef<SWidget> GenerateTransportWidget(TSharedPtr<FName> InItem) const;
	FText GetSelectedTransportText() const;

	ULiveLinkSourceFactory::FOnLiveLinkSourceCreated OnSourceCreated;
	/** The panel's own transient copy of the receiver defaults; the panel keeps it alive. */
	TStrongObjectPtr<UO3DReceiverSettingsObject> SourceSettingsObject;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SBox> TransportCustomizationContainer;
	/** Transport the options panel was built for; rebuilt only when it changes. */
	FName BuiltPanelTransport = NAME_None;
	TSharedPtr<SComboBox<TSharedPtr<FName>>> TransportComboBox;
	TArray<TSharedPtr<FName>> TransportOptions;
	TSharedPtr<SComboBox<TSharedPtr<FName>>> AudioCodecComboBox;
	TArray<TSharedPtr<FName>> AudioCodecOptions;
};
