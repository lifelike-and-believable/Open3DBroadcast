// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#include "O3DReceiverSourceFactory.h"

#include "O3DReceiverSource.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DReceiverTransportCustomization.h"

#include "CoreGlobals.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "O3DReceiverSourceFactory"

namespace
{
    static constexpr TCHAR ReceiverConnectionImportName[] = TEXT("UO3DReceiverSourceFactory");

    /** Set by Open3DBroadcastEditor (ADR 0010 §3). Read and written on the game thread only. */
    O3DReceiver::FSourceFactoryPanelBuilder& GetSourceFactoryPanelBuilder()
    {
        static O3DReceiver::FSourceFactoryPanelBuilder Builder;
        return Builder;
    }
}

void O3DReceiver::SetSourceFactoryPanelBuilder(FSourceFactoryPanelBuilder Builder)
{
    check(IsInGameThread());
    GetSourceFactoryPanelBuilder() = MoveTemp(Builder);
}

FText UO3DReceiverSourceFactory::GetSourceDisplayName() const
{
    return LOCTEXT("ReceiverSourceDisplayName", "Open3DStream Receiver");
}

FText UO3DReceiverSourceFactory::GetSourceTooltip() const
{
    return LOCTEXT("ReceiverSourceTooltip", "Receives Open3DStream subjects via configured transports.");
}

TSharedPtr<SWidget> UO3DReceiverSourceFactory::BuildCreationPanel(FOnLiveLinkSourceCreated InOnLiveLinkSourceCreated) const
{
    // The panel is editor UI and lives in Open3DBroadcastEditor, which sets the builder at startup.
    // Without it (a game, or the editor module disabled) there is no creation panel.
    const O3DReceiver::FSourceFactoryPanelBuilder& Builder = GetSourceFactoryPanelBuilder();
    return Builder ? Builder(MoveTemp(InOnLiveLinkSourceCreated)) : nullptr;
}

TSharedPtr<ILiveLinkSource> UO3DReceiverSourceFactory::CreateSource(const FString& ConnectionString) const
{
    // Migration (ADR 0004 item 4): an older GameUserSettings.ini or LiveLink connection string
    // may still carry a secret option. Move it to the session store and drop it here.
    UO3DReceiverSettingsObject* MutableDefaults = GetMutableDefault<UO3DReceiverSettingsObject>();
    O3DReceiver::MigrateLegacySecretOptions(MutableDefaults->Settings, TEXT("GameUserSettings.ini ([/Script/Open3DReceiver.O3DReceiverSettingsObject])"));
    FO3DReceiverSourceConfig Settings = MutableDefaults->Settings;

    if (!ConnectionString.IsEmpty())
    {
        FO3DReceiverSourceConfig::StaticStruct()->ImportText(*ConnectionString, &Settings, nullptr, PPF_None, GLog, ReceiverConnectionImportName);
        O3DReceiver::MigrateLegacySecretOptions(Settings, TEXT("a LiveLink connection string (for example a saved LiveLink preset)"));
    }

    TSharedPtr<FO3DReceiverSource> NewSource = MakeShared<FO3DReceiverSource>(Settings);
    return StaticCastSharedPtr<ILiveLinkSource>(NewSource);
}

#undef LOCTEXT_NAMESPACE
