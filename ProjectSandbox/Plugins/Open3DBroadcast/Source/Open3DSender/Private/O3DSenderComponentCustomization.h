// Copyright (c) Open3DStream Contributors

#pragma once

#if WITH_EDITOR

#include "IDetailCustomization.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"
#include "Widgets/SWidget.h"

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
    void HandleTransportConfigChanged();
    void RefreshTransportOptions();
    void RefreshTransportCustomization();
    void SyncTransportComboSelection();
    TSharedRef<SWidget> GenerateTransportWidget(TSharedPtr<FName> InItem) const;
    FText GetSelectedTransportText() const;
    FName GetSelectedTransportName() const;
    EVisibility GetTransportCustomizationVisibility() const;
    bool IsTransportSelectionEnabled() const;

    bool GetAutoCreateTransportValue(bool& bOutAutoCreate) const;

    TWeakObjectPtr<UO3DSenderComponent> WeakComponent;
    TSharedPtr<SBox> TransportCustomizationContainer;
    TSharedPtr<IPropertyHandle> AutoCreateTransportHandle;
    TSharedPtr<IPropertyHandle> TransportNameHandle;
    TSharedPtr<class SComboBox<TSharedPtr<FName>>> TransportComboBox;
    TArray<TSharedPtr<FName>> TransportOptions;
};

#endif // WITH_EDITOR
