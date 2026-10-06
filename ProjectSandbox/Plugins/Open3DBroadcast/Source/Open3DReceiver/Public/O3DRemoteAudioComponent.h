// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Components/SceneComponent.h"
#include "Containers/ArrayView.h"
#include "LiveLinkTypes.h"
#include "Engine/EngineTypes.h"
#include "O3DRemoteAudioComponent.generated.h"

class FO3DAudioJitterBuffer;
class FO3DRuntimeContext;

class UAudioComponent;
class USoundWaveProcedural;
class USoundAttenuation;
//struct FSoundAttenuationSettings;
class USoundSubmix;
class USoundEffectSourcePresetChain;
class USoundConcurrency;
struct FSoundSubmixSendInfo;
struct FSoundModulationDestinationSettings;
class USceneComponent;

namespace O3DS
{
    struct FAudioFrameMeta;
}

UENUM(BlueprintType)
enum class EO3DRemoteAudioMode : uint8
{
    /** Streams labelled "o3ds:mix": audio whose source set no stream id and whose transport no label. */
    Mix UMETA(DisplayName = "Mix (o3ds:mix)"),
    /** The stream of the LiveLink subject named in LiveLink Subject Name. */
    Subject UMETA(DisplayName = "Subject (LiveLink)"),
    /** Any stream (RCV-21). */
    AnyStream UMETA(DisplayName = "Any Stream")
};

UCLASS(ClassGroup = (Open3DBroadcast), meta = (BlueprintSpawnableComponent))
class OPEN3DRECEIVER_API UO3DRemoteAudioComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UO3DRemoteAudioComponent();

    /**
     * The runtime context whose audio this component plays (docs/adr/0012-runtime-services-and-global-state.md):
     * receiver sources with the same Context Name. Empty: the default context. Case-insensitive.
     * Read when play begins.
     */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Open3DBroadcast")
    FName ContextName;

    /** Select which remote audio stream to play. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio")
    EO3DRemoteAudioMode ReceiveMode = EO3DRemoteAudioMode::Mix;

    /** LiveLink Subject selector (visible only when ReceiveMode=Subject). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (EditCondition = "ReceiveMode == EO3DRemoteAudioMode::Subject", EditConditionHides))
    FLiveLinkSubjectName LiveLinkSubjectName;

    /**
     * Received audio held before playback starts, in ms (RCV-20). Absorbs network jitter; when more
     * than this plus 60 ms has built up (a sender clock running fast, a burst after a hitch), the
     * oldest audio is dropped back to it. Read when the first audio of a format arrives.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (ClampMin = "0.0", UIMax = "500.0", Units = "ms"))
    float TargetLatencyMs = 60.0f;

    /**
     * Plays only streams with this label (case-insensitive), in every mode. Empty: any label.
     * The component plays one stream at a time either way (RCV-21).
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio")
    FString StreamLabelFilter;

    /** Output gain applied to incoming samples prior to playback. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio")
    float Gain = 1.0f;

    /** Quick access mixers at top. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (DisplayName = "Volume Multiplier", ClampMin = "0.0", ToolTip = "Scales the overall output level of the audio component. 1.0 = original level."))
    float AC_VolumeMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (DisplayName = "Pitch Multiplier", ToolTip = "Scales playback pitch. 1.0 = original pitch."))
    float AC_PitchMultiplier = 1.0f;

    /** Attachment for the internally-created UAudioComponent (SceneComponent). */
    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Audio|Attachment", meta = (DisplayName = "Attach Parent", ToolTip = "Optional parent scene component to attach this component (and the audio it plays) to. If unset, the component stays where it is placed; one with no parent attaches to the Actor's RootComponent."))
    FComponentReference AC_AttachParent;

    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Audio|Attachment", meta = (DisplayName = "Attach Socket Name", ToolTip = "Optional socket to use when attaching to the parent component."))
    FName AC_AttachSocketName;

    /** AudioComponent configuration (applies to the auto-created component). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attenuation", meta = (DisplayName = "Allow Spatialization", ToolTip = "Whether to spatialize this sound when playing in 3D."))
    bool bAC_AllowSpatialization = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sound", meta = (DisplayName = "Is UI Sound", ToolTip = "If true, plays as a non-spatialized UI sound and may bypass reverb/occlusion."))
    bool bAC_IsUISound = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attenuation", meta = (DisplayName = "Override Attenuation", ToolTip = "Enable per-instance attenuation overrides below instead of using the asset's attenuation settings."))
    bool bAC_OverrideAttenuation = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attenuation", meta = (DisplayName = "Attenuation Settings", EditCondition = "!bAC_OverrideAttenuation", EditConditionHides, ToolTip = "Asset-based attenuation settings to apply when not overriding."))
    TObjectPtr<USoundAttenuation> AC_AttenuationSettings = nullptr;

    //UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attenuation", meta = (DisplayName = "Attenuation Overrides", EditCondition = "bAC_OverrideAttenuation", EditConditionHides, ToolTip = "Per-instance attenuation overrides used when Override Attenuation is enabled."))
    //FSoundAttenuationSettings AC_AttenuationOverrides;

    /** Submix Sends. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Submix", meta = (DisplayName = "Submix Sends", ToolTip = "Routes this source to one or more submixes using configurable send levels."))
    TArray<FSoundSubmixSendInfo> AC_SubmixSends;

    /** Source Effect Chain. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Effects", meta = (DisplayName = "Source Effect Chain", ToolTip = "Optional source effects chain to process this source."))
    TObjectPtr<USoundEffectSourcePresetChain> AC_SourceEffectChain = nullptr;

    /** Modulation (basic volume/pitch destinations). */
    //UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modulation", meta = (DisplayName = "Volume Modulation", ToolTip = "Modulation routing for source volume (if supported by this engine version)."))
    //FSoundModulationDestinationSettings AC_VolumeModulation;

    //UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modulation", meta = (DisplayName = "Pitch Modulation", ToolTip = "Modulation routing for source pitch (if supported by this engine version)."))
    //FSoundModulationDestinationSettings AC_PitchModulation;

    /** Concurrency. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Concurrency", meta = (DisplayName = "Concurrency Set", ToolTip = "Concurrency assets that limit how many instances of this sound can play."))
    TArray<TObjectPtr<USoundConcurrency>> AC_ConcurrencySet;

    // NOTE: Using asset-based concurrency (AC_ConcurrencySet) for all engine versions.
    // This provides consistent behavior across UE 5.6+ and avoids per-instance struct initialization issues in UE 5.7+.

    /** Auto-activate the internal audio component. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation", meta = (DisplayName = "Auto Activate", ToolTip = "If true, playback starts when the first audio arrives. If false, call Play."))
    bool bAC_AutoActivate = true;

    /** Starts playing received audio (now if some has arrived, otherwise when it does). */
    UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Audio")
    void Play();

    /** Stops playback. Audio that arrives afterwards is not played until Play is called. */
    UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Audio")
    void Stop();

protected:
    virtual void OnRegister() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    void OnAudioPcm16(const O3DS::FAudioFrameMeta& Meta, TConstArrayView<uint8> PCM16Bytes);
    void HandleAudioPcm16(const O3DS::FAudioFrameMeta& Meta, TConstArrayView<uint8> PCM16Bytes, double NowSeconds);
    /**
     * One stream at a time (RCV-21): the first matching stream (its source and label) is played
     * until it has sent nothing for StreamIdleReleaseSeconds; other streams are dropped meanwhile.
     */
    bool AcceptStream(const O3DS::FAudioFrameMeta& Meta, double NowSeconds);
    bool MatchesFilter(const FString& InSubject, const FString& InStream) const;
    /** Subscribes to the audio bus of ContextName's context, once; UnbindBus leaves that same bus. */
    void BindBus();
    void UnbindBus();
    void EnsureSoundWave(int32 NumChannels, int32 SampleRate);

    void AttachToConfiguredParent();

private:
    /** Created in BeginPlay, destroyed in EndPlay (RCV-23). */
    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> AudioComp = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<USoundWaveProcedural> SoundWave = nullptr;

    int32 CurrentChannels = 0;
    int32 CurrentSampleRate = 0;
    /** Between OnAudioPcm16 (game thread) and the sound wave (audio thread); one per wave. */
    TSharedPtr<FO3DAudioJitterBuffer, ESPMode::ThreadSafe> JitterBuffer;

    static constexpr double StreamIdleReleaseSeconds = 1.0;
    bool bStreamLocked = false;
    FGuid LockedSource;
    FString LockedLabel;
    double LockedLastPacketSeconds = 0.0;

    /** Set from bAC_AutoActivate at BeginPlay, then by Play and Stop. */
    bool bPlaybackWanted = false;

    FDelegateHandle BusDelegateHandle;
    /** The context BusDelegateHandle is bound in, so EndPlay unbinds from the same bus. */
    TSharedPtr<FO3DRuntimeContext, ESPMode::ThreadSafe> BoundContext;

    // Log throttling, per component (RCV-25): every component logs its own first frame.
    bool bLoggedFirstFrame = false;

    friend struct FO3DRemoteAudioComponentTestAccessor;
};
