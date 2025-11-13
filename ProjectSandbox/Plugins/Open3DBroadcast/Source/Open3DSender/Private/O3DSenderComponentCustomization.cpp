// Copyright (c) Open3DStream Contributors

#include "O3DSenderComponentCustomization.h"

#if WITH_EDITOR

#include "DetailCategoryBuilder.h"
#include "IDetailGroup.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "PropertyHandle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SNullWidget.h"

#include "Algo/Sort.h"

#include "O3DSenderComponent.h"
#include "O3DSenderTransportCustomization.h"

#define LOCTEXT_NAMESPACE "O3DSenderComponentCustomization"

namespace
{
FText GetSenderTransportDisplayName(FName TransportName)
{
    if (TransportName == FName(TEXT("sockets.tcp")))
    {
        return LOCTEXT("SenderTransportDisplayTcp", "TCP");
    }
    if (TransportName == FName(TEXT("sockets.udp")))
    {
        return LOCTEXT("SenderTransportDisplayUdp", "UDP");
    }

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
    if (Objects.Num() > 0)
    {
        WeakComponent = Cast<UO3DSenderComponent>(Objects[0].Get());
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
    TSharedPtr<IPropertyHandle> AudioStreamLabelHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, AudioStreamLabel));
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
    if (AudioStreamLabelHandle.IsValid())
    {
        DetailBuilder.HideProperty(AudioStreamLabelHandle);
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
        AudioGroup.AddPropertyRow(AudioInputDeviceHandle.ToSharedRef());
    }
    if (AudioStreamLabelHandle.IsValid())
    {
        AudioGroup.AddPropertyRow(AudioStreamLabelHandle.ToSharedRef());
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
}

void FO3DSenderComponentCustomization::HandleTransportSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type /*SelectInfo*/)
{
    if (!TransportNameHandle.IsValid() || !NewSelection.IsValid())
    {
        return;
    }

    TransportNameHandle->SetValue(*NewSelection);
}

void FO3DSenderComponentCustomization::HandleTransportPropertyChanged()
{
    RefreshTransportOptions();

    if (UO3DSenderComponent* Component = WeakComponent.Get())
    {
        Component->ClearTransportOptions();
    }

    RefreshTransportCustomization();
}

void FO3DSenderComponentCustomization::HandleTransportConfigChanged()
{
    RefreshTransportCustomization();
}

void FO3DSenderComponentCustomization::RefreshTransportOptions()
{
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
            TransportNameHandle->SetValue(CurrentSelection);
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
}

void FO3DSenderComponentCustomization::RefreshTransportCustomization()
{
    SyncTransportComboSelection();

    if (!TransportCustomizationContainer.IsValid())
    {
        return;
    }

    TransportCustomizationContainer->SetContent(SNullWidget::NullWidget);

    UO3DSenderComponent* Component = WeakComponent.Get();
    if (!Component)
    {
        return;
    }

    const FName TransportName = GetSelectedTransportName();
    if (TransportName.IsNone())
    {
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
                TransportCustomizationContainer->SetContent(CustomWidget.ToSharedRef());
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
}

void FO3DSenderComponentCustomization::SyncTransportComboSelection()
{
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
}

TSharedRef<SWidget> FO3DSenderComponentCustomization::GenerateTransportWidget(TSharedPtr<FName> InItem) const
{
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
    bool bAutoCreate = false;
    if (GetAutoCreateTransportValue(bAutoCreate))
    {
        return bAutoCreate;
    }

    return false;
}

bool FO3DSenderComponentCustomization::GetAutoCreateTransportValue(bool& bOutAutoCreate) const
{
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
    bool bAutoCreate = true;
    if (GetAutoCreateTransportValue(bAutoCreate))
    {
        return bAutoCreate ? EVisibility::Visible : EVisibility::Collapsed;
    }

    return EVisibility::Visible;
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
