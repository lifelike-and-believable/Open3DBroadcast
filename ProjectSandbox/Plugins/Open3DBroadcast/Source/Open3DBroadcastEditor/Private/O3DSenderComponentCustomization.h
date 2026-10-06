// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "IDetailCustomization.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/SWidget.h"
#include "Widgets/Input/SComboBox.h"

class UO3DSenderComponent;
class IDetailLayoutBuilder;
class IPropertyHandle;
class SBox;
class STextBlock;

class FO3DSenderComponentCustomization : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
    TSharedRef<FO3DSenderComponentCustomization> AsSharedCustomization()
    {
        return StaticCastSharedRef<FO3DSenderComponentCustomization>(IDetailCustomization::AsShared());
    }

    void HandleTransportSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo);
    void HandleTransportPropertyChanged();
    void RefreshTransportOptions();
    void RefreshTransportCustomization();
    void SyncTransportComboSelection();
    TSharedRef<SWidget> GenerateTransportWidget(TSharedPtr<FName> InItem) const;
    FText GetSelectedTransportText() const;
    FName GetSelectedTransportName() const;
    EVisibility GetTransportCustomizationVisibility() const;

    /**
     * The residual fallback warning for the edited component (ADR 0005 (iii)): empty unless
     * residual coding is enabled on a transport that does not deliver reliably and in order.
     * Recomputed at most every half second, since it builds a transport config.
     */
    FText GetResidualDeliveryWarning();
    FText CachedResidualWarning;
    double CachedResidualWarningTime = -1.0;
    bool IsTransportSelectionEnabled() const;

    bool GetAutoCreateTransportValue(bool& bOutAutoCreate) const;
    UO3DSenderComponent* ResolveEditingComponent();

    TWeakObjectPtr<UO3DSenderComponent> WeakComponent;
    TSharedPtr<SBox> TransportCustomizationContainer;
    TSharedPtr<IPropertyHandle> AutoCreateTransportHandle;
    TSharedPtr<IPropertyHandle> TransportNameHandle;
    TSharedPtr<SComboBox<TSharedPtr<FName>>> TransportComboBox;
    TArray<TSharedPtr<FName>> TransportOptions;
    /** Transport the current options panel was built for; the panel is rebuilt only when it changes. */
    FName BuiltPanelTransport = NAME_None;
    TWeakObjectPtr<UO3DSenderComponent> BuiltPanelComponent;
};
