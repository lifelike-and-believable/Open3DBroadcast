// Copyright (c) Open3DStream Contributors

#include "O3DSenderComponentCustomization.h"

#if WITH_EDITOR

#include "DetailCategoryBuilder.h"
#include "IDetailGroup.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Logging/LogMacros.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SNullWidget.h"

#include "Algo/Sort.h"

#include "O3DSenderComponent.h"
#include "O3DSenderTransportCustomization.h"

#define LOCTEXT_NAMESPACE "O3DSenderComponentCustomization"
DEFINE_LOG_CATEGORY_STATIC(LogO3DSenderDetails, Log, All);

namespace
{
FText GetSenderTransportDisplayName(FName TransportName)
{
    return FText::FromName(TransportName);
}
} // namespace

TSharedRef<IDetailCustomization> FO3DSenderComponentCustomization::MakeInstance()
{
    return MakeShared<FO3DSenderComponentCustomization>();
}

void FO3DSenderComponentCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
    TArray<TWeakObjectPtr<UObject>> Objects;
    DetailBuilder.GetObjectsBeingCustomized(Objects);
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("CustomizeDetails start: %d objects"), Objects.Num());
    for (int32 Index = 0; Index < Objects.Num(); ++Index)
    {
        if (UObject* Obj = Objects[Index].Get())
        {
            UE_LOG(LogO3DSenderDetails, Verbose, TEXT("  [%d] %s (%s)"), Index, *Obj->GetName(), *Obj->GetClass()->GetName());
        }
        else
        {
            UE_LOG(LogO3DSenderDetails, Warning, TEXT("  [%d] null object in customization list"), Index);
        }
    }
    WeakComponent.Reset();
    for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
    {
        if (UO3DSenderComponent* Candidate = Cast<UO3DSenderComponent>(WeakObj.Get()))
        {
            if (!WeakComponent.IsValid())
            {
                WeakComponent = Candidate;
            }

            if (!Candidate->HasAnyFlags(RF_ClassDefaultObject))
            {
                WeakComponent = Candidate;
                break;
            }
        }
    }

    if (WeakComponent.IsValid())
    {
        UE_LOG(LogO3DSenderDetails, Verbose, TEXT("Primary component: %s%s"),
            *WeakComponent->GetName(),
            WeakComponent->HasAnyFlags(RF_ClassDefaultObject) ? TEXT(" [CDO]") : TEXT(""));
    }
    else
    {
        UE_LOG(LogO3DSenderDetails, Warning, TEXT("CustomizeDetails could not resolve a valid sender component."));
    }

    DetailBuilder.HideCategory(TEXT("Open3DStream|Sender"));
    DetailBuilder.HideCategory(TEXT("Open3DStream|Sender|Transport"));
    DetailBuilder.HideCategory(TEXT("Open3DStream|Sender|Audio"));
    DetailBuilder.HideCategory(TEXT("Open3DStream|Audio"));
    DetailBuilder.HideCategory(TEXT("Open3DStream|Sender|Curves"));
    DetailBuilder.HideCategory(TEXT("Open3DStream|Sender|Curves|Filtering"));
    DetailBuilder.HideCategory(TEXT("Audio"));

    TSharedPtr<IPropertyHandle> TargetMeshHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TargetMesh));
    TSharedPtr<IPropertyHandle> SubjectNameHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, SubjectName));
    TSharedPtr<IPropertyHandle> CaptureRateHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, CaptureRateHz));
    TSharedPtr<IPropertyHandle> AutoStartHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bAutoStartCapture));

    AutoCreateTransportHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bAutoCreateTransport));
    TransportNameHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName));

    TSharedPtr<IPropertyHandle> EnableAudioHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bEnableAudio));
    TSharedPtr<IPropertyHandle> AudioCaptureModeHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureMode));
    TSharedPtr<IPropertyHandle> AudioCodecHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCodec));
        TSharedPtr<IPropertyHandle> ClampCurvesHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bClampMorphCurvesToUnit));
        TSharedPtr<IPropertyHandle> DropNaNHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bDropNaNAndInfinity));
        TSharedPtr<IPropertyHandle> EnableCurveFilteringHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bEnableCurveFiltering));
        TSharedPtr<IPropertyHandle> CurveEpsilonHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, CurveEpsilon));
        TSharedPtr<IPropertyHandle> CurveDeltaThresholdHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, CurveDeltaThreshold));
        TSharedPtr<IPropertyHandle> IncludeCurvePatternsHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, IncludeCurvePatterns));
        TSharedPtr<IPropertyHandle> ExcludeCurvePatternsHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, ExcludeCurvePatterns));
        TSharedPtr<IPropertyHandle> LogFilteredCurvesHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bLogFilteredCurves));

        TArray<TSharedPtr<IPropertyHandle>> CommonHandles = {
            TargetMeshHandle,
            SubjectNameHandle,
            CaptureRateHandle,
            AutoStartHandle,
            ClampCurvesHandle,
            DropNaNHandle,
            EnableCurveFilteringHandle,
            CurveEpsilonHandle,
            CurveDeltaThresholdHandle,
            IncludeCurvePatternsHandle,
            ExcludeCurvePatternsHandle,
            LogFilteredCurvesHandle
        };

        for (const TSharedPtr<IPropertyHandle>& Handle : CommonHandles)
        {
            if (Handle.IsValid())
            {
                DetailBuilder.HideProperty(Handle);
            }
        }

    TSharedPtr<IPropertyHandle> AudioInputDeviceHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioInputDevice));
    TSharedPtr<IPropertyHandle> AudioCaptureConfigHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioCaptureConfig));

    if (EnableAudioHandle.IsValid())
    {
        DetailBuilder.HideProperty(EnableAudioHandle);
    }
    if (AudioCaptureModeHandle.IsValid())
    {
        DetailBuilder.HideProperty(AudioCaptureModeHandle);
    }
    if (AudioInputDeviceHandle.IsValid())
    {
        DetailBuilder.HideProperty(AudioInputDeviceHandle);
    }
    if (AudioCodecHandle.IsValid())
    {
        DetailBuilder.HideProperty(AudioCodecHandle);
    }
    if (AudioCaptureConfigHandle.IsValid())
    {
        DetailBuilder.HideProperty(AudioCaptureConfigHandle);
        AudioCaptureConfigHandle->MarkHiddenByCustomization();
    }

    if (AutoCreateTransportHandle.IsValid())
    {
        DetailBuilder.HideProperty(AutoCreateTransportHandle);
    }

    if (TransportNameHandle.IsValid())
    {
        DetailBuilder.HideProperty(TransportNameHandle);
        TransportNameHandle->SetOnPropertyValueChanged(FSimpleDelegate::CreateSP(AsSharedCustomization(), &FO3DSenderComponentCustomization::HandleTransportPropertyChanged));
    }

    if (TSharedPtr<IPropertyHandle> TransportOptionsHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportOptions)))
    {
        DetailBuilder.HideProperty(TransportOptionsHandle);
    }

    RefreshTransportOptions();

    IDetailCategoryBuilder& RootCategory = DetailBuilder.EditCategory(TEXT("Open3DStream"));
    RootCategory.SetDisplayName(LOCTEXT("RootCategoryLabel", "Open3DStream"));
    RootCategory.SetSortOrder(0);

    IDetailGroup& SenderGroup = RootCategory.AddGroup(TEXT("Sender"), LOCTEXT("SenderGroupLabel", "Sender"), false, true);

    if (TargetMeshHandle.IsValid())
    {
        SenderGroup.AddPropertyRow(TargetMeshHandle.ToSharedRef());
    }
    if (SubjectNameHandle.IsValid())
    {
        SenderGroup.AddPropertyRow(SubjectNameHandle.ToSharedRef());
    }
    if (CaptureRateHandle.IsValid())
    {
        SenderGroup.AddPropertyRow(CaptureRateHandle.ToSharedRef());
    }
    if (AutoStartHandle.IsValid())
    {
        SenderGroup.AddPropertyRow(AutoStartHandle.ToSharedRef());
    }

    IDetailGroup& TransportGroup = RootCategory.AddGroup(TEXT("Transport"), LOCTEXT("TransportGroupLabel", "Transport"), false, true);

    if (AutoCreateTransportHandle.IsValid())
    {
        TransportGroup.AddPropertyRow(AutoCreateTransportHandle.ToSharedRef());
    }

    if (TransportNameHandle.IsValid())
    {
        TransportGroup.AddWidgetRow()
        .NameContent()
        [
            TransportNameHandle->CreatePropertyNameWidget()
        ]
        .ValueContent()
        .MinDesiredWidth(250.f)
        .MaxDesiredWidth(0.f)
        [
            SAssignNew(TransportComboBox, SComboBox<TSharedPtr<FName>>)
            .IsEnabled(this, &FO3DSenderComponentCustomization::IsTransportSelectionEnabled)
            .OptionsSource(&TransportOptions)
            .OnGenerateWidget(this, &FO3DSenderComponentCustomization::GenerateTransportWidget)
            .OnSelectionChanged(this, &FO3DSenderComponentCustomization::HandleTransportSelectionChanged)
            .OnComboBoxOpening(FSimpleDelegate::CreateSP(AsSharedCustomization(), &FO3DSenderComponentCustomization::RefreshTransportOptions))
            [
                SNew(STextBlock)
                .Text(this, &FO3DSenderComponentCustomization::GetSelectedTransportText)
                .Font(IDetailLayoutBuilder::GetDetailFont())
            ]
        ];
    }

    TransportGroup.AddWidgetRow()
    .WholeRowContent()
    [
        SAssignNew(TransportCustomizationContainer, SBox)
        .Padding(FMargin(0.f, 4.f))
        .Visibility_Lambda([this]() { return GetTransportCustomizationVisibility(); })
    ];

    IDetailGroup& CurvesGroup = RootCategory.AddGroup(TEXT("Curves"), LOCTEXT("CurvesGroupLabel", "Curves"), false, true);

    if (ClampCurvesHandle.IsValid())
    {
        CurvesGroup.AddPropertyRow(ClampCurvesHandle.ToSharedRef());
    }
    if (DropNaNHandle.IsValid())
    {
        CurvesGroup.AddPropertyRow(DropNaNHandle.ToSharedRef());
    }

    IDetailGroup& CurveFilteringGroup = CurvesGroup.AddGroup(TEXT("CurveFiltering"), LOCTEXT("CurveFilteringGroupLabel", "Filtering"), false);
    if (EnableCurveFilteringHandle.IsValid())
    {
        CurveFilteringGroup.AddPropertyRow(EnableCurveFilteringHandle.ToSharedRef());
    }
    if (CurveEpsilonHandle.IsValid())
    {
        CurveFilteringGroup.AddPropertyRow(CurveEpsilonHandle.ToSharedRef());
    }
    if (CurveDeltaThresholdHandle.IsValid())
    {
        CurveFilteringGroup.AddPropertyRow(CurveDeltaThresholdHandle.ToSharedRef());
    }
    if (IncludeCurvePatternsHandle.IsValid())
    {
        CurveFilteringGroup.AddPropertyRow(IncludeCurvePatternsHandle.ToSharedRef());
    }
    if (ExcludeCurvePatternsHandle.IsValid())
    {
        CurveFilteringGroup.AddPropertyRow(ExcludeCurvePatternsHandle.ToSharedRef());
    }
    if (LogFilteredCurvesHandle.IsValid())
    {
        CurveFilteringGroup.AddPropertyRow(LogFilteredCurvesHandle.ToSharedRef());
    }

    IDetailGroup& AudioGroup = RootCategory.AddGroup(TEXT("Audio"), LOCTEXT("AudioGroupLabel", "Audio"), false, true);

    if (EnableAudioHandle.IsValid())
    {
        AudioGroup.AddPropertyRow(EnableAudioHandle.ToSharedRef());
    }
    if (AudioCaptureModeHandle.IsValid())
    {
        AudioGroup.AddPropertyRow(AudioCaptureModeHandle.ToSharedRef());
    }
    if (AudioInputDeviceHandle.IsValid())
    {
        const TWeakPtr<FO3DSenderComponentCustomization> CustomizationWeak = AsSharedCustomization();
        IDetailPropertyRow& AudioInputRow = AudioGroup.AddPropertyRow(AudioInputDeviceHandle.ToSharedRef());
        AudioInputRow.Visibility(TAttribute<EVisibility>::CreateLambda([CustomizationWeak]()
        {
            const TSharedPtr<FO3DSenderComponentCustomization> Customization = CustomizationWeak.Pin();
            if (!Customization.IsValid())
            {
                return EVisibility::Collapsed;
            }

            if (const UO3DSenderComponent* Component = Customization->ResolveEditingComponent())
            {
                const bool bEnableAudioValue = Component->bEnableAudio;
                const bool bMicMode = (Component->AudioCaptureMode == EO3DSenderCaptureMode::Input);
                UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("AudioInputDevice visibility (component state): Enable=%d Mode=%d -> %s"),
                    bEnableAudioValue,
                    static_cast<int32>(Component->AudioCaptureMode),
                    (bEnableAudioValue && bMicMode) ? TEXT("Visible") : TEXT("Collapsed"));

                return (bEnableAudioValue && bMicMode) ? EVisibility::Visible : EVisibility::Collapsed;
            }

            UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("AudioInputDevice visibility: component unavailable"));
            return EVisibility::Collapsed;
        }));
    }
    if (AudioCodecHandle.IsValid())
    {
        AudioGroup.AddPropertyRow(AudioCodecHandle.ToSharedRef());
    }
    if (AudioCaptureConfigHandle.IsValid())
    {
        IDetailGroup& AudioConfigGroup = AudioGroup.AddGroup(TEXT("AudioCaptureConfig"), LOCTEXT("AudioCaptureConfigLabel", "Audio Capture Config"), false);

        uint32 NumChildren = 0;
        if (AudioCaptureConfigHandle->GetNumChildren(NumChildren) == FPropertyAccess::Success)
        {
            for (uint32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
            {
                TSharedPtr<IPropertyHandle> ChildHandle = AudioCaptureConfigHandle->GetChildHandle(ChildIndex);
                if (ChildHandle.IsValid())
                {
                    ChildHandle->MarkHiddenByCustomization();
                    AudioConfigGroup.AddPropertyRow(ChildHandle.ToSharedRef());
                }
            }
        }
    }

    RefreshTransportCustomization();
    SyncTransportComboSelection();

    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("CustomizeDetails completed"));
}

void FO3DSenderComponentCustomization::HandleTransportSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type /*SelectInfo*/)
{
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("HandleTransportSelectionChanged: %s"),
        (NewSelection.IsValid() ? *NewSelection->ToString() : TEXT("<none>")));
    if (!TransportNameHandle.IsValid() || !NewSelection.IsValid())
    {
        return;
    }

    FName ExistingValue = NAME_None;
    if (TransportNameHandle->GetValue(ExistingValue) == FPropertyAccess::Success && ExistingValue == *NewSelection)
    {
        UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("Transport selection unchanged, skipping SetValue"));
        return;
    }

    TransportNameHandle->SetValue(*NewSelection);
}

void FO3DSenderComponentCustomization::HandleTransportPropertyChanged()
{
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("HandleTransportPropertyChanged"));
    RefreshTransportOptions();

    if (UO3DSenderComponent* Component = ResolveEditingComponent())
    {
        Component->ClearTransportOptions();
    }

    RefreshTransportCustomization();
}

void FO3DSenderComponentCustomization::HandleTransportConfigChanged()
{
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("HandleTransportConfigChanged"));
    RefreshTransportCustomization();
}

void FO3DSenderComponentCustomization::RefreshTransportOptions()
{
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("RefreshTransportOptions begin"));
    TransportOptions.Reset();

    TArray<FName> RegisteredTransports;
    O3DSender::GetRegisteredTransportNames(RegisteredTransports);

    for (const FName& Name : RegisteredTransports)
    {
        TransportOptions.Add(MakeShared<FName>(Name));
    }

    FName CurrentSelection = GetSelectedTransportName();

    if (CurrentSelection.IsNone() && TransportOptions.Num() > 0)
    {
        CurrentSelection = *TransportOptions[0];
        if (TransportNameHandle.IsValid())
        {
            FName ExistingValue = NAME_None;
            const bool bHasValue = (TransportNameHandle->GetValue(ExistingValue) == FPropertyAccess::Success);
            if (!bHasValue || ExistingValue.IsNone())
            {
                TransportNameHandle->SetValue(CurrentSelection);
            }
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

    SyncTransportComboSelection();
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("RefreshTransportOptions end: %d options"), TransportOptions.Num());
}

void FO3DSenderComponentCustomization::RefreshTransportCustomization()
{
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("RefreshTransportCustomization begin"));
    SyncTransportComboSelection();

    if (!TransportCustomizationContainer.IsValid())
    {
        UE_LOG(LogO3DSenderDetails, Warning, TEXT("TransportCustomizationContainer invalid"));
        return;
    }

    TransportCustomizationContainer->SetContent(SNullWidget::NullWidget);

    UO3DSenderComponent* Component = ResolveEditingComponent();
    if (!Component)
    {
        UE_LOG(LogO3DSenderDetails, Verbose, TEXT("No valid component while refreshing transport customization; clearing widget"));
        TransportCustomizationContainer->SetContent(SNullWidget::NullWidget);
        return;
    }

    const FName TransportName = GetSelectedTransportName();
    if (TransportName.IsNone())
    {
        UE_LOG(LogO3DSenderDetails, Verbose, TEXT("No transport selected; skipping customization widget"));
        return;
    }

    if (const FO3DSenderTransportCustomization* Customization = O3DSender::FindTransportCustomization(TransportName))
    {
#if WITH_EDITOR
        if (Customization && Customization->BuildTransportWidget)
        {
            FSimpleDelegate OnConfigChanged = FSimpleDelegate::CreateSP(AsSharedCustomization(), &FO3DSenderComponentCustomization::HandleTransportConfigChanged);
            if (TSharedPtr<SWidget> CustomWidget = Customization->BuildTransportWidget(Component, OnConfigChanged))
            {
                UE_LOG(LogO3DSenderDetails, Verbose, TEXT("Built transport widget for %s"), *TransportName.ToString());
                TransportCustomizationContainer->SetContent(CustomWidget.ToSharedRef());
                UE_LOG(LogO3DSenderDetails, Verbose, TEXT("RefreshTransportCustomization end (custom widget)"));
                return;
            }
        }
#endif // WITH_EDITOR
    }

    TransportCustomizationContainer->SetContent(
        SNew(STextBlock)
        .Text(NSLOCTEXT("O3DSenderCustomization", "NoTransportOptions", "No transport-specific settings available."))
        .Font(IDetailLayoutBuilder::GetDetailFontItalic())
    );
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("Transport customization widget not available for %s"), *TransportName.ToString());
    UE_LOG(LogO3DSenderDetails, Verbose, TEXT("RefreshTransportCustomization end (fallback text)"));
}

void FO3DSenderComponentCustomization::SyncTransportComboSelection()
{
    UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("SyncTransportComboSelection begin"));
    if (!TransportComboBox.IsValid())
    {
        return;
    }

    const FName CurrentSelection = GetSelectedTransportName();

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
    UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("SyncTransportComboSelection end (%s)"),
        MatchingItem.IsValid() ? *MatchingItem->ToString() : TEXT("<none>"));
}

TSharedRef<SWidget> FO3DSenderComponentCustomization::GenerateTransportWidget(TSharedPtr<FName> InItem) const
{
    UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("GenerateTransportWidget for %s"),
        InItem.IsValid() ? *InItem->ToString() : TEXT("<none>"));
    const FText Label = InItem.IsValid() ? GetSenderTransportDisplayName(*InItem) : FText::GetEmpty();
    return SNew(STextBlock)
        .Text(Label)
        .Font(IDetailLayoutBuilder::GetDetailFont());
}

FText FO3DSenderComponentCustomization::GetSelectedTransportText() const
{
    const FName Transport = GetSelectedTransportName();
    return Transport.IsNone()
        ? NSLOCTEXT("O3DSenderCustomization", "TransportNoneSelected", "Select Transport")
        : GetSenderTransportDisplayName(Transport);
}

FName FO3DSenderComponentCustomization::GetSelectedTransportName() const
{
    UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("GetSelectedTransportName invoked"));
    if (TransportNameHandle.IsValid())
    {
        FName PropertyValue = NAME_None;
        if (TransportNameHandle->GetValue(PropertyValue) == FPropertyAccess::Success)
        {
            return PropertyValue;
        }
    }

    if (const UO3DSenderComponent* Component = WeakComponent.Get())
    {
        return Component->GetTransportName();
    }

    return NAME_None;
}

bool FO3DSenderComponentCustomization::IsTransportSelectionEnabled() const
{
    UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("IsTransportSelectionEnabled invoked"));
    bool bAutoCreate = false;
    if (GetAutoCreateTransportValue(bAutoCreate))
    {
        return bAutoCreate;
    }

    return false;
}

bool FO3DSenderComponentCustomization::GetAutoCreateTransportValue(bool& bOutAutoCreate) const
{
    UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("GetAutoCreateTransportValue invoked"));
    if (AutoCreateTransportHandle.IsValid())
    {
        bool bValue = false;
        const FPropertyAccess::Result Result = AutoCreateTransportHandle->GetValue(bValue);
        if (Result == FPropertyAccess::Success)
        {
            bOutAutoCreate = bValue;
            return true;
        }

        if (Result == FPropertyAccess::MultipleValues)
        {
            return false;
        }
    }

    if (const UO3DSenderComponent* Component = WeakComponent.Get())
    {
        bOutAutoCreate = Component->bAutoCreateTransport;
        return true;
    }

    return false;
}

EVisibility FO3DSenderComponentCustomization::GetTransportCustomizationVisibility() const
{
    UE_LOG(LogO3DSenderDetails, VeryVerbose, TEXT("GetTransportCustomizationVisibility invoked"));
    bool bAutoCreate = true;
    if (GetAutoCreateTransportValue(bAutoCreate))
    {
        return bAutoCreate ? EVisibility::Visible : EVisibility::Collapsed;
    }

    return EVisibility::Visible;
}

UO3DSenderComponent* FO3DSenderComponentCustomization::ResolveEditingComponent()
{
    if (WeakComponent.IsStale())
    {
        WeakComponent.Reset();
    }

    if (WeakComponent.IsValid())
    {
        return WeakComponent.Get();
    }

    if (TransportNameHandle.IsValid())
    {
        TArray<UObject*> OuterObjects;
        TransportNameHandle->GetOuterObjects(OuterObjects);
        for (UObject* Outer : OuterObjects)
        {
            if (UO3DSenderComponent* Component = Cast<UO3DSenderComponent>(Outer))
            {
                WeakComponent = Component;
                return Component;
            }
        }
    }

    return nullptr;
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
