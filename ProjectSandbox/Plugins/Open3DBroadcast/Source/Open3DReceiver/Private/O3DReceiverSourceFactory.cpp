// Copyright (c) Open3DStream Contributors

#include "O3DReceiverSourceFactory.h"

#include "O3DReceiverSource.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DTransportConfigPanelBase.h"

#include "UObject/UnrealType.h"
#include "Widgets/SNullWidget.h"

#define LOCTEXT_NAMESPACE "O3DReceiverSourceFactory"

#if WITH_EDITOR
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "PropertyHandle.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "DetailLayoutBuilder.h"
#include "InputCoreTypes.h"
#include "Framework/Application/SlateApplication.h"
#include "Algo/Sort.h"
#include "O3DReceiverTransportCustomization.h"

using FReceiverSourceCreated = ULiveLinkSourceFactory::FOnLiveLinkSourceCreated;
#endif // WITH_EDITOR

namespace
{
    static constexpr TCHAR ReceiverConnectionImportName[] = TEXT("UO3DReceiverSourceFactory");

#if WITH_EDITOR
    FText GetReceiverTransportDisplayName(FName TransportName)
    {
        return FText::FromName(TransportName);
    }
#endif // WITH_EDITOR
}

#if WITH_EDITOR
class SO3DReceiverSourceFactoryPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SO3DReceiverSourceFactoryPanel) {}
        SLATE_ARGUMENT(FReceiverSourceCreated, OnSourceCreated)
    SLATE_END_ARGS()

    virtual bool SupportsKeyboardFocus() const override { return true; }

    void Construct(const FArguments& InArgs)
    {
        OnSourceCreated = InArgs._OnSourceCreated;

        SourceSettingsObject = DuplicateObject<UO3DReceiverSettingsObject>(GetMutableDefault<UO3DReceiverSettingsObject>(), GetTransientPackage());
        if (!SourceSettingsObject)
        {
            SourceSettingsObject = NewObject<UO3DReceiverSettingsObject>(GetTransientPackage());
        }

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
        DetailsView->SetObject(SourceSettingsObject);
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

private:
    FReply OnCreateClicked()
    {
        if (!OnSourceCreated.IsBound() || !SourceSettingsObject)
        {
            return FReply::Handled();
        }

        const FO3DReceiverSourceConfig& Settings = SourceSettingsObject->Settings;

        FString ConnectionString;
        FO3DReceiverSourceConfig::StaticStruct()->ExportText(ConnectionString, &Settings, nullptr, nullptr, PPF_None, nullptr);

        UO3DReceiverSettingsObject* MutableDefaults = GetMutableDefault<UO3DReceiverSettingsObject>();
        MutableDefaults->Settings = Settings;
        MutableDefaults->SaveConfig();

        TSharedPtr<FO3DReceiverSource> NewSource = MakeShared<FO3DReceiverSource>(Settings);
        OnSourceCreated.ExecuteIfBound(StaticCastSharedPtr<ILiveLinkSource>(NewSource), MoveTemp(ConnectionString));

        return FReply::Handled();
    }

private:
    virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
    {
        const FKey Key = InKeyEvent.GetKey();
        if (Key == EKeys::Enter || Key == EKeys::Virtual_Accept)
        {
            return OnCreateClicked();
        }

        return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
    }

    void HandleSettingsPropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent)
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

    bool HandleIsPropertyVisible(const FPropertyAndParent& PropertyAndParent) const
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

    void RefreshTransportCustomization()
    {
        SyncTransportSelection();

        if (!TransportCustomizationContainer.IsValid())
        {
            return;
        }

        TransportCustomizationContainer->SetContent(SNullWidget::NullWidget);

        const FName TransportName = GetCurrentTransportName();
        if (TransportName.IsNone())
        {
            return;
        }

        if (const FO3DReceiverTransportCustomization* Customization = O3DReceiver::FindTransportCustomization(TransportName))
        {
            if (Customization && Customization->BuildTransportWidget)
            {
                const FSimpleDelegate SubmitDelegate = FSimpleDelegate::CreateSP(this, &SO3DReceiverSourceFactoryPanel::HandleCustomizationSubmit);
                if (TSharedPtr<SO3DTransportConfigPanelBase> CustomPanel = Customization->BuildTransportWidget(SourceSettingsObject, SubmitDelegate))
                {
                    CustomPanel->SetPanelWidth(SO3DTransportConfigPanelBase::DefaultPanelWidth);
                    TransportCustomizationContainer->SetContent(CustomPanel.ToSharedRef());
                }
            }
        }
    }

    void HandleCustomizationSubmit()
    {
        OnCreateClicked();
    }

    FName GetCurrentTransportName() const
    {
        return SourceSettingsObject ? SourceSettingsObject->Settings.TransportName : NAME_None;
    }

    void RefreshAudioCodecOptions()
    {
        AudioCodecOptions.Reset();

        AudioCodecOptions.Add(MakeShared<FName>(NAME_None));
        AudioCodecOptions.Add(MakeShared<FName>(FName(TEXT("pcm16"))));
#if O3D_WITH_OPUS
        AudioCodecOptions.Add(MakeShared<FName>(FName(TEXT("opus"))));
#endif

        if (AudioCodecComboBox.IsValid())
        {
            AudioCodecComboBox->RefreshOptions();
            SyncAudioCodecSelection();
        }
    }

    void SyncAudioCodecSelection()
    {
        if (!AudioCodecComboBox.IsValid())
        {
            return;
        }

        const FName CurrentCodec = SourceSettingsObject ? SourceSettingsObject->Settings.AudioCodec : NAME_None;

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

    void HandleAudioCodecSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type /*SelectInfo*/)
    {
        if (!SourceSettingsObject)
        {
            return;
        }

        const FName SelectedCodec = (NewSelection.IsValid() ? *NewSelection : NAME_None);
        if (SourceSettingsObject->Settings.AudioCodec != SelectedCodec)
        {
            SourceSettingsObject->Modify();
            SourceSettingsObject->Settings.AudioCodec = SelectedCodec;
        }
    }

    TSharedRef<SWidget> GenerateAudioCodecWidget(TSharedPtr<FName> InItem) const
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

    FText GetSelectedAudioCodecText() const
    {
        const FName CurrentCodec = SourceSettingsObject ? SourceSettingsObject->Settings.AudioCodec : NAME_None;
        if (CurrentCodec.IsNone())
        {
            return LOCTEXT("ReceiverAudioCodecDefaultLabel", "Transport Default");
        }

        return FText::FromName(CurrentCodec);
    }

    bool IsAudioCodecSelectionEnabled() const
    {
        return SourceSettingsObject && SourceSettingsObject->Settings.bEnableAudio;
    }

    void RefreshTransportOptions()
    {
        TransportOptions.Reset();

        TArray<FName> RegisteredTransports;
        O3DReceiver::GetRegisteredTransportNames(RegisteredTransports);

        for (const FName& Name : RegisteredTransports)
        {
            TransportOptions.Add(MakeShared<FName>(Name));
        }

        FName CurrentSelection = GetCurrentTransportName();
        if (CurrentSelection.IsNone() && TransportOptions.Num() > 0)
        {
            CurrentSelection = *TransportOptions[0];
            if (SourceSettingsObject)
            {
                SourceSettingsObject->Settings.TransportName = CurrentSelection;
            }
        }

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

    void SyncTransportSelection()
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

        if (!MatchingItem.IsValid() && TransportOptions.Num() > 0)
        {
            MatchingItem = TransportOptions[0];
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

    void HandleTransportSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type /*SelectInfo*/)
    {
        if (!SourceSettingsObject || !NewSelection.IsValid())
        {
            return;
        }

        if (SourceSettingsObject->Settings.TransportName != *NewSelection)
        {
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

    TSharedRef<SWidget> GenerateTransportWidget(TSharedPtr<FName> InItem) const
    {
        const FText Label = InItem.IsValid() ? GetReceiverTransportDisplayName(*InItem) : FText::GetEmpty();
        return SNew(STextBlock)
            .Text(Label)
            .Font(IDetailLayoutBuilder::GetDetailFont());
    }

    FText GetSelectedTransportText() const
    {
        const FName CurrentSelection = GetCurrentTransportName();
        return CurrentSelection.IsNone()
            ? LOCTEXT("ReceiverTransportSelectPrompt", "Select Transport")
            : GetReceiverTransportDisplayName(CurrentSelection);
    }

    FReceiverSourceCreated OnSourceCreated;
    UO3DReceiverSettingsObject* SourceSettingsObject = nullptr;
    TSharedPtr<IDetailsView> DetailsView;
    TSharedPtr<SBox> TransportCustomizationContainer;
    TSharedPtr<SComboBox<TSharedPtr<FName>>> TransportComboBox;
    TArray<TSharedPtr<FName>> TransportOptions;
    TSharedPtr<SComboBox<TSharedPtr<FName>>> AudioCodecComboBox;
    TArray<TSharedPtr<FName>> AudioCodecOptions;
};
#endif // WITH_EDITOR

FText UO3DReceiverSourceFactory::GetSourceDisplayName() const
{
    return LOCTEXT("ReceiverSourceDisplayName", "Open3DStream Receiver");
}

FText UO3DReceiverSourceFactory::GetSourceTooltip() const
{
    return LOCTEXT("ReceiverSourceTooltip", "Receives Open3DStream subjects via configured transports.");
}

TSharedPtr<SWidget> UO3DReceiverSourceFactory::BuildCreationPanel(FOnLiveLinkSourceCreated InOnLiveLinkSourceCreated) const
{
#if WITH_EDITOR
    return SNew(SO3DReceiverSourceFactoryPanel)
        .OnSourceCreated(InOnLiveLinkSourceCreated);
#else
    return SNullWidget::NullWidget;
#endif // WITH_EDITOR
}

TSharedPtr<ILiveLinkSource> UO3DReceiverSourceFactory::CreateSource(const FString& ConnectionString) const
{
    FO3DReceiverSourceConfig Settings = GetDefault<UO3DReceiverSettingsObject>()->Settings;

    if (!ConnectionString.IsEmpty())
    {
        FO3DReceiverSourceConfig::StaticStruct()->ImportText(*ConnectionString, &Settings, nullptr, PPF_None, GLog, ReceiverConnectionImportName);
    }

    TSharedPtr<FO3DReceiverSource> NewSource = MakeShared<FO3DReceiverSource>(Settings);
    return StaticCastSharedPtr<ILiveLinkSource>(NewSource);
}

#undef LOCTEXT_NAMESPACE
