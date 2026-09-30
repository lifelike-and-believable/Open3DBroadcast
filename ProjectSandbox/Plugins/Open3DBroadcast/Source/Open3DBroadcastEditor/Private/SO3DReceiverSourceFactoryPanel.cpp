// Copyright Lifelike & Believable. All Rights Reserved.

#include "SO3DReceiverSourceFactoryPanel.h"

#include "DetailLayoutBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "IDetailsView.h"
#include "InputCoreTypes.h"
#include "Modules/ModuleManager.h"
#include "O3DReceiverSource.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DTransportOptionTarget.h"
#include "PropertyEditorModule.h"
#include "SO3DTransportOptionsPanel.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "O3DReceiverSourceFactory"

namespace
{
	FText GetReceiverTransportDisplayName(FName TransportName)
	{
		return FText::FromName(TransportName);
	}
}

void SO3DReceiverSourceFactoryPanel::Construct(const FArguments& InArgs)
{
	OnSourceCreated = InArgs._OnSourceCreated;

	// The panel's own copy of the defaults. RF_Transactional so transport and option edits in the
	// panel can be undone. Nothing is saved until Create Source.
	UO3DReceiverSettingsObject* Copy = DuplicateObject<UO3DReceiverSettingsObject>(GetMutableDefault<UO3DReceiverSettingsObject>(), GetTransientPackage());
	if (!Copy)
	{
		Copy = NewObject<UO3DReceiverSettingsObject>(GetTransientPackage());
	}
	Copy->SetFlags(RF_Transactional);

	// An ini with no transport gets the first registered one. This changes only the panel's copy.
	if (Copy->Settings.TransportName.IsNone())
	{
		TArray<FName> RegisteredTransports;
		O3DReceiver::GetRegisteredTransportNames(RegisteredTransports);
		if (RegisteredTransports.Num() > 0)
		{
			Copy->Settings.TransportName = RegisteredTransports[0];
		}
	}
	SourceSettingsObject = TStrongObjectPtr<UO3DReceiverSettingsObject>(Copy);

	RefreshTransportOptions();
	RefreshAudioCodecOptions();

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = true;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Hide;
	DetailsArgs.NotifyHook = nullptr;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	DetailsView = PropertyModule.CreateDetailView(DetailsArgs);
	DetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateSP(this, &SO3DReceiverSourceFactoryPanel::HandleIsPropertyVisible));
	DetailsView->SetObject(SourceSettingsObject.Get());
	DetailsView->OnFinishedChangingProperties().AddSP(this, &SO3DReceiverSourceFactoryPanel::HandleSettingsPropertyChanged);

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ReceiverTransportLabel", "Transport"))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(TransportComboBox, SComboBox<TSharedPtr<FName>>)
				.OptionsSource(&TransportOptions)
				.OnGenerateWidget(this, &SO3DReceiverSourceFactoryPanel::GenerateTransportWidget)
				.OnSelectionChanged(this, &SO3DReceiverSourceFactoryPanel::HandleTransportSelectionChanged)
				.OnComboBoxOpening(FSimpleDelegate::CreateSP(this, &SO3DReceiverSourceFactoryPanel::RefreshTransportOptions))
				[
					SNew(STextBlock)
					.Text(this, &SO3DReceiverSourceFactoryPanel::GetSelectedTransportText)
					.Font(IDetailLayoutBuilder::GetDetailFont())
				]
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ReceiverAudioCodecLabel", "Audio Codec"))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(AudioCodecComboBox, SComboBox<TSharedPtr<FName>>)
				.OptionsSource(&AudioCodecOptions)
				.IsEnabled(this, &SO3DReceiverSourceFactoryPanel::IsAudioCodecSelectionEnabled)
				.OnGenerateWidget(this, &SO3DReceiverSourceFactoryPanel::GenerateAudioCodecWidget)
				.OnSelectionChanged(this, &SO3DReceiverSourceFactoryPanel::HandleAudioCodecSelectionChanged)
				[
					SNew(STextBlock)
					.Text(this, &SO3DReceiverSourceFactoryPanel::GetSelectedAudioCodecText)
					.Font(IDetailLayoutBuilder::GetDetailFont())
				]
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			DetailsView.ToSharedRef()
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SAssignNew(TransportCustomizationContainer, SBox)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Right)
		[
			SNew(SUniformGridPanel)
			+ SUniformGridPanel::Slot(0, 0)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("ReceiverCreateSource", "Create Source"))
				.OnClicked(this, &SO3DReceiverSourceFactoryPanel::OnCreateClicked)
			]
		]
	];

	RefreshTransportOptions();
	SyncTransportSelection();
	SyncAudioCodecSelection();
	RefreshTransportCustomization();

	FSlateApplication::Get().SetKeyboardFocus(AsShared(), EFocusCause::SetDirectly);
}

FReply SO3DReceiverSourceFactoryPanel::OnCreateClicked()
{
	if (!OnSourceCreated.IsBound() || !SourceSettingsObject.IsValid())
	{
		return FReply::Handled();
	}

	// Declared secret keys never reach the connection string or GameUserSettings.ini
	// (ADR 0004 item 4); the source resolves them from FO3DSecretStore when it starts.
	FO3DReceiverSourceConfig Settings = SourceSettingsObject->Settings;
	O3DReceiver::MigrateLegacySecretOptions(Settings, TEXT("the receiver source settings"));

	FString ConnectionString = O3DReceiver::ExportConnectionString(Settings);

	UO3DReceiverSettingsObject* MutableDefaults = GetMutableDefault<UO3DReceiverSettingsObject>();
	MutableDefaults->Settings = Settings;
	MutableDefaults->SaveConfig();

	TSharedPtr<FO3DReceiverSource> NewSource = MakeShared<FO3DReceiverSource>(Settings);
	OnSourceCreated.ExecuteIfBound(StaticCastSharedPtr<ILiveLinkSource>(NewSource), MoveTemp(ConnectionString));

	return FReply::Handled();
}

FReply SO3DReceiverSourceFactoryPanel::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	const FKey Key = InKeyEvent.GetKey();
	if (Key == EKeys::Enter || Key == EKeys::Virtual_Gamepad_Accept.GetVirtualKey())
	{
		return OnCreateClicked();
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SO3DReceiverSourceFactoryPanel::HandleSettingsPropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(FO3DReceiverSourceConfig, TransportName))
	{
		RefreshTransportOptions();
		RefreshTransportCustomization();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(FO3DReceiverSourceConfig, AudioCodec)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(FO3DReceiverSourceConfig, bEnableAudio))
	{
		SyncAudioCodecSelection();
	}
}

bool SO3DReceiverSourceFactoryPanel::HandleIsPropertyVisible(const FPropertyAndParent& PropertyAndParent) const
{
	const FProperty& Property = PropertyAndParent.Property;

	const FName PropertyName = Property.GetFName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(FO3DReceiverSourceConfig, TransportOptions) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(FO3DReceiverSourceConfig, TransportName) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(FO3DReceiverSourceConfig, AudioCodec))
	{
		return false;
	}

	return true;
}

void SO3DReceiverSourceFactoryPanel::RefreshTransportCustomization()
{
	SyncTransportSelection();

	if (!TransportCustomizationContainer.IsValid())
	{
		return;
	}

	const FName TransportName = GetCurrentTransportName();
	if (TransportName == BuiltPanelTransport && !TransportName.IsNone())
	{
		// The options panel reads through bound attributes; no rebuild for the same transport.
		return;
	}

	TransportCustomizationContainer->SetContent(SNullWidget::NullWidget);
	BuiltPanelTransport = NAME_None;

	if (TransportName.IsNone() || !SourceSettingsObject.IsValid())
	{
		return;
	}

	// ADR 0010 §4: the transport declares its options as data; one generic panel renders them.
	FO3DTransportOptionSchema Schema;
	if (O3DReceiver::GetTransportOptionSchema(TransportName, Schema) && Schema.Num() > 0)
	{
		TransportCustomizationContainer->SetContent(
			SNew(SO3DTransportOptionsPanel)
			.Target(MakeShared<FO3DReceiverOptionTarget>(SourceSettingsObject.Get()))
			.Schema(MoveTemp(Schema))
			.PanelWidth(SO3DTransportConfigPanelBase::DefaultPanelWidth)
			.OnSubmit(FSimpleDelegate::CreateSP(this, &SO3DReceiverSourceFactoryPanel::HandleOptionsSubmit))
		);
		BuiltPanelTransport = TransportName;
	}
}

void SO3DReceiverSourceFactoryPanel::HandleOptionsSubmit()
{
	OnCreateClicked();
}

FName SO3DReceiverSourceFactoryPanel::GetCurrentTransportName() const
{
	return SourceSettingsObject.IsValid() ? SourceSettingsObject->Settings.TransportName : NAME_None;
}

void SO3DReceiverSourceFactoryPanel::RefreshAudioCodecOptions()
{
	AudioCodecOptions.Reset();

	AudioCodecOptions.Add(MakeShared<FName>(NAME_None));
	AudioCodecOptions.Add(MakeShared<FName>(FName(TEXT("PCM16"))));
#if O3D_WITH_OPUS
	AudioCodecOptions.Add(MakeShared<FName>(FName(TEXT("Opus"))));
#endif

	if (AudioCodecComboBox.IsValid())
	{
		AudioCodecComboBox->RefreshOptions();
		SyncAudioCodecSelection();
	}
}

void SO3DReceiverSourceFactoryPanel::SyncAudioCodecSelection()
{
	if (!AudioCodecComboBox.IsValid())
	{
		return;
	}

	const FName CurrentCodec = SourceSettingsObject.IsValid() ? SourceSettingsObject->Settings.AudioCodec : NAME_None;

	TSharedPtr<FName> MatchingItem;
	for (const TSharedPtr<FName>& Option : AudioCodecOptions)
	{
		if (Option.IsValid() && *Option == CurrentCodec)
		{
			MatchingItem = Option;
			break;
		}
	}

	if (!MatchingItem.IsValid() && AudioCodecOptions.Num() > 0)
	{
		MatchingItem = AudioCodecOptions[0];
	}

	if (MatchingItem.IsValid())
	{
		AudioCodecComboBox->SetSelectedItem(MatchingItem);
	}
	else
	{
		AudioCodecComboBox->ClearSelection();
	}
}

void SO3DReceiverSourceFactoryPanel::HandleAudioCodecSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo)
{
	// Direct is SyncAudioCodecSelection mirroring the stored value, not a user edit.
	if (!SourceSettingsObject.IsValid() || SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	const FName SelectedCodec = (NewSelection.IsValid() ? *NewSelection : NAME_None);
	if (SourceSettingsObject->Settings.AudioCodec != SelectedCodec)
	{
		const FScopedTransaction Transaction(LOCTEXT("ReceiverChangeAudioCodec", "Change Receiver Audio Codec"));
		SourceSettingsObject->Modify();
		SourceSettingsObject->Settings.AudioCodec = SelectedCodec;
	}
}

TSharedRef<SWidget> SO3DReceiverSourceFactoryPanel::GenerateAudioCodecWidget(TSharedPtr<FName> InItem) const
{
	const FName CodecName = InItem.IsValid() ? *InItem : NAME_None;
	FText Label;
	if (CodecName.IsNone())
	{
		Label = LOCTEXT("ReceiverAudioCodecDefault", "Transport Default");
	}
	else
	{
		Label = FText::FromName(CodecName);
	}

	return SNew(STextBlock)
		.Text(Label)
		.Font(IDetailLayoutBuilder::GetDetailFont());
}

FText SO3DReceiverSourceFactoryPanel::GetSelectedAudioCodecText() const
{
	const FName CurrentCodec = SourceSettingsObject.IsValid() ? SourceSettingsObject->Settings.AudioCodec : NAME_None;
	if (CurrentCodec.IsNone())
	{
		return LOCTEXT("ReceiverAudioCodecDefaultLabel", "Transport Default");
	}

	return FText::FromName(CurrentCodec);
}

bool SO3DReceiverSourceFactoryPanel::IsAudioCodecSelectionEnabled() const
{
	return SourceSettingsObject.IsValid() && SourceSettingsObject->Settings.bEnableAudio;
}

void SO3DReceiverSourceFactoryPanel::RefreshTransportOptions()
{
	TransportOptions.Reset();

	TArray<FName> RegisteredTransports;
	O3DReceiver::GetRegisteredTransportNames(RegisteredTransports);

	for (const FName& Name : RegisteredTransports)
	{
		TransportOptions.Add(MakeShared<FName>(Name));
	}

	// Read only (TRB-45): a transport that is no longer registered stays listed so the current
	// choice is visible.
	const FName CurrentSelection = GetCurrentTransportName();
	if (CurrentSelection != NAME_None)
	{
		const bool bAlreadyIncluded = TransportOptions.ContainsByPredicate([&CurrentSelection](const TSharedPtr<FName>& Option)
		{
			return Option.IsValid() && *Option == CurrentSelection;
		});

		if (!bAlreadyIncluded)
		{
			TransportOptions.Add(MakeShared<FName>(CurrentSelection));
		}
	}

	TransportOptions.Sort([](const TSharedPtr<FName>& A, const TSharedPtr<FName>& B)
	{
		if (!A.IsValid())
		{
			return false;
		}
		if (!B.IsValid())
		{
			return true;
		}
		return FNameLexicalLess()(*A, *B);
	});

	if (TransportComboBox.IsValid())
	{
		TransportComboBox->RefreshOptions();
	}

	SyncTransportSelection();
}

void SO3DReceiverSourceFactoryPanel::SyncTransportSelection()
{
	if (!TransportComboBox.IsValid())
	{
		return;
	}

	const FName CurrentSelection = GetCurrentTransportName();

	TSharedPtr<FName> MatchingItem;
	for (const TSharedPtr<FName>& Option : TransportOptions)
	{
		if (Option.IsValid() && *Option == CurrentSelection)
		{
			MatchingItem = Option;
			break;
		}
	}

	if (MatchingItem.IsValid())
	{
		TransportComboBox->SetSelectedItem(MatchingItem);
	}
	else
	{
		TransportComboBox->ClearSelection();
	}
}

void SO3DReceiverSourceFactoryPanel::HandleTransportSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo)
{
	// Direct is SyncTransportSelection mirroring the stored value, not a user edit.
	if (!SourceSettingsObject.IsValid() || !NewSelection.IsValid() || SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	if (SourceSettingsObject->Settings.TransportName != *NewSelection)
	{
		// One undoable edit. The options are cleared because most keys ("host", "port") are not
		// namespaced by transport yet; ADR 0007 item 8 (WP-A1) namespaces them and drops the clear.
		const FScopedTransaction Transaction(LOCTEXT("ReceiverChangeTransport", "Change Receiver Transport"));
		SourceSettingsObject->Modify();
		SourceSettingsObject->Settings.TransportName = *NewSelection;
		SourceSettingsObject->Settings.TransportOptions.Empty();

		if (DetailsView.IsValid())
		{
			DetailsView->ForceRefresh();
		}

		RefreshTransportCustomization();
	}

	SyncTransportSelection();
}

TSharedRef<SWidget> SO3DReceiverSourceFactoryPanel::GenerateTransportWidget(TSharedPtr<FName> InItem) const
{
	const FText Label = InItem.IsValid() ? GetReceiverTransportDisplayName(*InItem) : FText::GetEmpty();
	return SNew(STextBlock)
		.Text(Label)
		.Font(IDetailLayoutBuilder::GetDetailFont());
}

FText SO3DReceiverSourceFactoryPanel::GetSelectedTransportText() const
{
	const FName CurrentSelection = GetCurrentTransportName();
	return CurrentSelection.IsNone()
		? LOCTEXT("ReceiverTransportSelectPrompt", "Select Transport")
		: GetReceiverTransportDisplayName(CurrentSelection);
}

#undef LOCTEXT_NAMESPACE
