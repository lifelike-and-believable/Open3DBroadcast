#pragma once

#if WITH_EDITOR

#include "IDetailCustomization.h"

class IDetailLayoutBuilder;

class FO3DSBroadcastComponentCustomization : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance();

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;
};

#endif // WITH_EDITOR
