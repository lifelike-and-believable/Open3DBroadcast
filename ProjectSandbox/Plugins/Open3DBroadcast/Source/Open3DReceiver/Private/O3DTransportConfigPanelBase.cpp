// Copyright (c) Open3DStream Contributors

#include "O3DTransportConfigPanelBase.h"

#include "InputCoreTypes.h"
#include "Math/UnrealMathUtility.h"
#include "Widgets/Layout/SBox.h"

SO3DTransportConfigPanelBase::SO3DTransportConfigPanelBase()
    : PanelWidth(DefaultPanelWidth)
{
}

void SO3DTransportConfigPanelBase::BuildPanel(const TSharedRef<SWidget>& Content, float WidthOverride)
{
    PanelWidth = SanitizeWidth(WidthOverride);

    ChildSlot
    [
        SAssignNew(PanelContainer, SBox)
        .WidthOverride(PanelWidth)
        [
            Content
        ]
    ];
}

void SO3DTransportConfigPanelBase::SetPanelWidth(float InWidth)
{
    PanelWidth = SanitizeWidth(InWidth);
    if (PanelContainer.IsValid())
    {
        PanelContainer->SetWidthOverride(PanelWidth);
    }
}

void SO3DTransportConfigPanelBase::SetOnSubmit(FSimpleDelegate InOnSubmit)
{
    SubmitDelegate = InOnSubmit;
}

void SO3DTransportConfigPanelBase::Submit()
{
    if (SubmitDelegate.IsBound())
    {
        SubmitDelegate.Execute();
    }
}

FReply SO3DTransportConfigPanelBase::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
    const FKey Key = InKeyEvent.GetKey();
    if (Key == EKeys::Enter || Key == EKeys::Virtual_Accept || Key == EKeys::Gamepad_FaceButton_Bottom)
    {
        Submit();
        return FReply::Handled();
    }

    return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SO3DTransportConfigPanelBase::SubmitFromTextCommit(ETextCommit::Type CommitType)
{
    if (CommitType == ETextCommit::OnEnter)
    {
        Submit();
    }
}

float SO3DTransportConfigPanelBase::SanitizeWidth(float InWidth) const
{
    return FMath::Max(MinimumPanelWidth, InWidth > 0.f ? InWidth : DefaultPanelWidth);
}
