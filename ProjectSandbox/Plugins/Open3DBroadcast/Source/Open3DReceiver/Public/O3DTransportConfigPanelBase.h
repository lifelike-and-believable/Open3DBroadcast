// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Types/SlateEnums.h"

class SBox;

/**
 * Base panel used by transport-specific LiveLink configuration widgets to ensure
 * consistent sizing and submission behavior across transports.
 */
class OPEN3DRECEIVER_API SO3DTransportConfigPanelBase : public SCompoundWidget
{
public:
    static constexpr float DefaultPanelWidth = 360.f;
    static constexpr float MinimumPanelWidth = 280.f;

    SO3DTransportConfigPanelBase();

    virtual bool SupportsKeyboardFocus() const override { return true; }

    /** Wrap the provided content in a width-constrained SBox and assign it to the child slot. */
    void BuildPanel(const TSharedRef<SWidget>& Content, float WidthOverride = DefaultPanelWidth);

    /** Update the enforced panel width at runtime. */
    void SetPanelWidth(float InWidth);
    float GetPanelWidth() const { return PanelWidth; }

    /** Bind the delegate invoked when the user submits the panel (e.g. presses Enter). */
    void SetOnSubmit(FSimpleDelegate InOnSubmit);

protected:
    /** Manually trigger the submit delegate if it is bound. */
    void Submit();

    virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

    /** Helper for text commit handlers to submit on Enter. */
    void SubmitFromTextCommit(ETextCommit::Type CommitType);

private:
    float SanitizeWidth(float InWidth) const;

    TSharedPtr<SBox> PanelContainer;
    FSimpleDelegate SubmitDelegate;
    float PanelWidth;
};
