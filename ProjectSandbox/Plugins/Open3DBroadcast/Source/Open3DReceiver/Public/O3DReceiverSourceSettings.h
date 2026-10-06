// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "LiveLinkSourceSettings.h"
#include "Transport/O3DTransportOptionSet.h"
#include "O3DReceiverSourceSettings.generated.h"

/**
 * Whether one receiver source accepts control messages (docs/adr/0011-control-channel.md,
 * item 8). ProjectDefault follows the runtime override (UO3DControlLibrary) if one is set, and
 * otherwise the project setting (UO3DControlSettings::bAcceptControl).
 */
UENUM(BlueprintType)
enum class EO3DControlAcceptMode : uint8
{
    ProjectDefault UMETA(DisplayName = "Project Default"),
    Enabled,
    Disabled
};

/** User-facing configuration used to bootstrap receiver transports inside LiveLink. */
USTRUCT(BlueprintType)
struct FO3DReceiverSourceConfig
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast")
    FName TransportName = TEXT("loopback");

    /**
     * The runtime context this source publishes its audio, control and metrics to
     * (docs/adr/0012-runtime-services-and-global-state.md). Empty: the default context, which
     * every component with an empty Context Name listens to. Remote audio and control components
     * with the same name hear this source; others do not. Case-insensitive. Applies when the source
     * is created.
     */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast", AdvancedDisplay)
    FName ContextName;

    /** Enable audio playback for transports that support it. */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Audio")
    bool bEnableAudio = false;

    /** Preferred decoder codec; leave empty to use transport default. */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Audio", meta = (EditCondition = "bEnableAudio", EditConditionHides))
    FName AudioCodec = NAME_None;

    /** Transport-specific key/value overrides populated by modular transport UIs. Hidden from the generic details panel. */
    UPROPERTY(VisibleAnywhere, Category = "Open3DBroadcast", meta = (HideInDetailPanel))
    TMap<FString, FString> TransportOptions;

    /**
     * The options of transports these settings used before, by transport name (SND-35, WP-A1 PR
     * 5a). Changing the transport in the source panel puts TransportOptions here and switching
     * back restores them. Never holds a secret (ADR 0004). Not shown in any panel.
     */
    UPROPERTY()
    TMap<FName, FO3DTransportOptionSet> InactiveTransportOptions;
};

/**
 * The settings of one new receiver source while the LiveLink Create Source panel edits them.
 * Not a defaults object: project-wide defaults are in Project Settings > Plugins > Open3DBroadcast
 * (UOpen3DBroadcastSettings), and nothing saves this class any more (RCV-18). It stays
 * Config = GameUserSettings only so CreateSource can move secrets an older version saved to that
 * file into the secret store and rewrite the file without them (ADR 0004 item 4).
 */
UCLASS(Config = GameUserSettings)
class OPEN3DRECEIVER_API UO3DReceiverSettingsObject : public UObject
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, GlobalConfig, Category = "Open3DBroadcast", Meta = (ShowOnlyInnerProperties))
    FO3DReceiverSourceConfig Settings;
};

/** Per-source-instance settings shown in LiveLink's "Settings" panel for this receiver source.
 *  Concealment (roadmap doc §5/C1) config lives here rather than as cvars - these are production
 *  tuning knobs a project would set per-deployment, not debug/iteration toggles, so they belong in
 *  the UI a user actually configures a LiveLink source from, not a console command. */
UCLASS()
class OPEN3DRECEIVER_API UO3DReceiverSourceSettings : public ULiveLinkSourceSettings
{
    GENERATED_BODY()

public:
    /**
     * Seconds without a frame after which a subject's frames are cleared, so LiveLink shows it as
     * having no data. The subject and its LiveLink settings (preprocessors, interpolation,
     * translators) are kept, and its next frame makes it valid again. 0: never.
     */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast", meta = (ClampMin = "0.0", Units = "s"))
    float InactiveSubjectTimeoutSeconds = 5.0f;

    /** Enable receiver-side concealment (predict/hold synthetic frames on a gap) for gated (A2) frames. */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Concealment")
    bool bEnableConcealment = true;

    /** Gap since the last real frame (ms) beyond which concealment starts synthesizing, instead of
     *  leaving small gaps to LiveLink's own interpolation. */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Concealment", meta = (EditCondition = "bEnableConcealment", ClampMin = "0.0"))
    float StarvationThresholdMs = 50.0f;

    /** Stop extrapolating and hold after this many ms of continuous concealment with no real frame. */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Concealment", meta = (EditCondition = "bEnableConcealment", ClampMin = "0.0"))
    float MaxHorizonMs = 150.0f;

    /** Blend from the last synthesized pose toward the resumed real trajectory over this many ms
     *  after a gap recovers, instead of snapping. 0 disables correction blending. */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Concealment", meta = (EditCondition = "bEnableConcealment", ClampMin = "0.0"))
    float CorrectionWindowMs = 100.0f;

    /** Latency-hiding horizon (ms, roadmap doc §5/C1.c) beyond the newest real frame to proactively
     *  predict toward, even with no gap. 0 (default) disables render-ahead entirely - it trades
     *  prediction accuracy for lower perceived latency, so it's opt-in. */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Concealment", meta = (EditCondition = "bEnableConcealment", ClampMin = "0.0"))
    float RenderAheadMs = 0.0f;

    /**
     * Whether this source accepts control messages (events and values). Project Default follows
     * the runtime override, then Project Settings > Plugins > Open3DBroadcast Control. Enabled or
     * Disabled here wins over both, for projects that want control on only some sources.
     */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Control")
    EO3DControlAcceptMode ControlAccept = EO3DControlAcceptMode::ProjectDefault;
};
