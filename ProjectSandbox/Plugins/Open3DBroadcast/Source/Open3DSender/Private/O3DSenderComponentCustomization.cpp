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

TSharedRef<IDetailCustomization> FO3DSenderComponentCustomization::MakeInstance()
{
    return MakeShared<FO3DSenderComponentCustomization>();
}

void FO3DSenderComponentCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
    CachedDetailBuilder = &DetailBuilder;

    TArray<TWeakObjectPtr<UObject>> Objects;
    DetailBuilder.GetObjectsBeingCustomized(Objects);
    if (Objects.Num() > 0)
    {
        WeakComponent = Cast<UO3DSenderComponent>(Objects[0].Get());
    }

    IDetailCategoryBuilder& SenderCategory = DetailBuilder.EditCategory(TEXT("Open3DStream|Sender"));
    SenderCategory.SetSortOrder(0);
    SenderCategory.InitiallyCollapsed(false);

    DetailBuilder.HideCategory(TEXT("Open3DStream|Sender|Transport"));

    TSharedPtr<IPropertyHandle> TargetMeshHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TargetMesh));
    TSharedPtr<IPropertyHandle> SubjectNameHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, SubjectName));
    TSharedPtr<IPropertyHandle> CaptureRateHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, CaptureRateHz));
    TSharedPtr<IPropertyHandle> AutoStartHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bAutoStartCapture));

    TArray<TSharedPtr<IPropertyHandle>> CommonHandles = {
        TargetMeshHandle,
        SubjectNameHandle,
        CaptureRateHandle,
        AutoStartHandle
    };

    for (const TSharedPtr<IPropertyHandle>& Handle : CommonHandles)
    {
        if (Handle.IsValid())
        {
            DetailBuilder.HideProperty(Handle);
            SenderCategory.AddProperty(Handle);
        }
    }

    AutoCreateTransportHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, bAutoCreateTransport));
    TransportNameHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UO3DSenderComponent, TransportName));

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

    IDetailGroup& TransportGroup = SenderCategory.AddGroup(TEXT("TransportConfig"), NSLOCTEXT("O3DSenderCustomization", "TransportGroupLabel", "Transport"), false, false);

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

    if (AutoCreateTransportHandle.IsValid())
    {
        TransportGroup.AddPropertyRow(AutoCreateTransportHandle.ToSharedRef());
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

    if (CachedDetailBuilder)
    {
        CachedDetailBuilder->ForceRefreshDetails();
    }
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
    const FText Label = InItem.IsValid() ? FText::FromName(*InItem) : FText::GetEmpty();
    return SNew(STextBlock)
        .Text(Label)
        .Font(IDetailLayoutBuilder::GetDetailFont());
}

FText FO3DSenderComponentCustomization::GetSelectedTransportText() const
{
    const FName Transport = GetSelectedTransportName();
    return Transport.IsNone()
        ? NSLOCTEXT("O3DSenderCustomization", "TransportNoneSelected", "Select Transport")
        : FText::FromName(Transport);
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

EVisibility FO3DSenderComponentCustomization::GetTransportCustomizationVisibility() const
{
    if (AutoCreateTransportHandle.IsValid())
    {
        bool bAutoCreate = true;
        if (AutoCreateTransportHandle->GetValue(bAutoCreate) == FPropertyAccess::Success && !bAutoCreate)
        {
            return EVisibility::Collapsed;
        }
    }

    return EVisibility::Visible;
}

#endif // WITH_EDITOR
