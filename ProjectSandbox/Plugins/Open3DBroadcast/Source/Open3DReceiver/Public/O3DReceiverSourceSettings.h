// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "LiveLinkSourceSettings.h"
#include "O3DReceiverSourceSettings.generated.h"

/** User-facing configuration used to bootstrap receiver transports inside LiveLink. */
USTRUCT(BlueprintType)
struct FO3DReceiverSourceConfig
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "Open3DStream")
    FName TransportName = TEXT("loopback");

    /** Enable audio playback for transports that support it. */
    UPROPERTY(EditAnywhere, Category = "Open3DStream|Audio")
    bool bEnableAudio = false;

    /** Optional stream label override applied to published audio frames. Defaults to transport-provided value. */
    UPROPERTY(EditAnywhere, Category = "Open3DStream|Audio", meta = (EditCondition = "bEnableAudio", EditConditionHides))
    FString AudioStreamLabel;

    /** Transport-specific key/value overrides populated by modular transport UIs. Hidden from the generic details panel. */
    UPROPERTY(VisibleAnywhere, Category = "Open3DStream", meta = (HideInDetailPanel))
    TMap<FString, FString> TransportOptions;
};

/** Global config object that exposes default receiver settings via the Project Settings UI. */
UCLASS(Config = GameUserSettings)
class OPEN3DRECEIVER_API UO3DReceiverSettingsObject : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, GlobalConfig, Category = "Open3DStream", Meta = (ShowOnlyInnerProperties))
    FO3DReceiverSourceConfig Settings;
};

UCLASS()
class OPEN3DRECEIVER_API UO3DReceiverSourceSettings : public ULiveLinkSourceSettings
{
    GENERATED_BODY()
};
