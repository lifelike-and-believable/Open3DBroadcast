// Copyright (c) Open3DStream Contributors

#pragma once

#include "LiveLinkSourceFactory.h"
#include "O3DReceiverSourceFactory.generated.h"

class ILiveLinkSource;

/**
 * LiveLink factory entry that exposes the Open3DStream receiver source in the editor UI.
 */
UCLASS()
class OPEN3DRECEIVER_API UO3DReceiverSourceFactory : public ULiveLinkSourceFactory
{
    GENERATED_BODY()

public:
    virtual FText GetSourceDisplayName() const override;
    virtual FText GetSourceTooltip() const override;
    virtual EMenuType GetMenuType() const override { return EMenuType::SubPanel; }
    virtual TSharedPtr<SWidget> BuildCreationPanel(FOnLiveLinkSourceCreated InOnLiveLinkSourceCreated) const override;
    virtual TSharedPtr<ILiveLinkSource> CreateSource(const FString& ConnectionString) const override;
};
