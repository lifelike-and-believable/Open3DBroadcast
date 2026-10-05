// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "LiveLinkSourceFactory.h"
#include "Templates/Function.h"
#include "O3DReceiverSourceFactory.generated.h"

class ILiveLinkSource;
class SWidget;

/**
 * LiveLink factory entry that exposes the Open3DStream receiver source in the editor UI.
 *
 * Stays in this runtime module: applying a LiveLink preset in a game calls CreateSource. The
 * creation panel is editor UI, built by the Open3DBroadcastEditor module through
 * O3DReceiver::SetSourceFactoryPanelBuilder (ADR 0010 §3).
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

namespace O3DReceiver
{
    /** Builds the LiveLink "Add Source" panel for UO3DReceiverSourceFactory. */
    using FSourceFactoryPanelBuilder = TFunction<TSharedPtr<SWidget>(ULiveLinkSourceFactory::FOnLiveLinkSourceCreated)>;

    /**
     * Sets the builder UO3DReceiverSourceFactory::BuildCreationPanel calls. The Open3DBroadcastEditor
     * module sets it in StartupModule and clears it (nullptr) in ShutdownModule; without a builder
     * BuildCreationPanel returns nullptr. Game thread only.
     */
    OPEN3DRECEIVER_API void SetSourceFactoryPanelBuilder(FSourceFactoryPanelBuilder Builder);
}
