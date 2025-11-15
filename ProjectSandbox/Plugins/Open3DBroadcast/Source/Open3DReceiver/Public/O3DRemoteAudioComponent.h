// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "LiveLinkTypes.h"
#include "Engine/EngineTypes.h"
#include "O3DRemoteAudioComponent.generated.h"

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
    Mix UMETA(DisplayName = "Mix (o3ds:mix)"),
    Subject UMETA(DisplayName = "Subject (LiveLink)")
};

UCLASS(ClassGroup = (Open3DStream), meta = (BlueprintSpawnableComponent))
class OPEN3DRECEIVER_API UO3DRemoteAudioComponent : public USceneComponent
{
    GENERATED_BODY()

public:
    UO3DRemoteAudioComponent();

    /** Select which remote audio stream to play. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    EO3DRemoteAudioMode ReceiveMode = EO3DRemoteAudioMode::Mix;

    /** LiveLink Subject selector (visible only when ReceiveMode=Subject). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio", meta = (EditCondition = "ReceiveMode == EO3DRemoteAudioMode::Subject", EditConditionHides))
    FLiveLinkSubjectName LiveLinkSubjectName;

    /** Optional stream label substring filter (advanced). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    FString StreamLabelFilter;

    /** Output gain applied to incoming samples prior to playback. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    float Gain = 1.0f;

    /** Quick access mixers at top. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio", meta = (DisplayName = "Volume Multiplier", ClampMin = "0.0", ToolTip = "Scales the overall output level of the audio component. 1.0 = original level."))
    float AC_VolumeMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio", meta = (DisplayName = "Pitch Multiplier", ToolTip = "Scales playback pitch. 1.0 = original pitch."))
    float AC_PitchMultiplier = 1.0f;

    /** Attachment for the internally-created UAudioComponent (SceneComponent). */
    UPROPERTY(EditAnywhere, Category = "Open3DStream|Audio|Attachment", meta = (DisplayName = "Attach Parent", ToolTip = "Optional parent scene component to attach the internal UAudioComponent to. If unset, attaches to the Actor's RootComponent."))
    FComponentReference AC_AttachParent;

    UPROPERTY(EditAnywhere, Category = "Open3DStream|Audio|Attachment", meta = (DisplayName = "Attach Socket Name", ToolTip = "Optional socket to use when attaching to the parent component."))
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
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation", meta = (DisplayName = "Auto Activate", ToolTip = "If true, the internal audio component auto-starts when the procedural sound is ready."))
    bool bAC_AutoActivate = true;

protected:
    virtual void OnRegister() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    void OnAudioFrame(const FString& StreamLabel, const FString& SubjectName, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate);
    void OnAudioPcm16(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes);
    bool MatchesFilter(const FString& InSubject, const FString& InStream) const;
    void EnsureSoundWave(int32 NumChannels, int32 SampleRate);

    void AttachToConfiguredParent();

private:
    UAudioComponent* AudioComp = nullptr;

    UPROPERTY(Transient)
    USoundWaveProcedural* SoundWave = nullptr;

    int32 CurrentChannels = 0;
    int32 CurrentSampleRate = 0;
    bool bOwnsAudioComponent = false;

    FDelegateHandle BusDelegateHandle;

    friend struct FO3DRemoteAudioComponentTestAccessor;
};
