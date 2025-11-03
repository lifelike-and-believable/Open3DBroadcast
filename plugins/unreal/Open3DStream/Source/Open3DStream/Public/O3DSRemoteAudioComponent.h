// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LiveLinkTypes.h"
#include "O3DSRemoteAudioComponent.generated.h"

class UAudioComponent;
class USoundWaveProcedural;
class USoundAttenuation;
struct FSoundAttenuationSettings;
class USoundSubmix;
class USoundEffectSourcePresetChain;
class USoundConcurrency;
struct FSoundSubmixSendInfo;
struct FSoundConcurrencySettings;
struct FSoundModulationDestinationSettings;

namespace O3DS { struct FAudioFrameMeta; }

UENUM(BlueprintType)
enum class EO3DSRemoteAudioMode : uint8
{
	Mix UMETA(DisplayName="Mix (o3ds:mix)"),
	Subject UMETA(DisplayName="Subject (LiveLink)")
};

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DSTREAM_API UO3DSRemoteAudioComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UO3DSRemoteAudioComponent();

	// Select which remote audio to play
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	EO3DSRemoteAudioMode ReceiveMode = EO3DSRemoteAudioMode::Mix;

	// LiveLink Subject selector (visible only when ReceiveMode=Subject)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio", meta=(EditCondition="ReceiveMode == EO3DSRemoteAudioMode::Subject", EditConditionHides))
	FLiveLinkSubjectName LiveLinkSubjectName;

	// Optional stream label substring filter (advanced)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	FString StreamLabelFilter;

	// Output gain
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	float Gain =1.0f;

	// AudioComponent configuration (applies to the auto-created component)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent")
	bool bAC_AllowSpatialization = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent")
	bool bAC_IsUISound = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent", meta=(ClampMin="0.0"))
	float AC_VolumeMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent")
	float AC_PitchMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent")
	bool bAC_OverrideAttenuation = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent", meta=(EditCondition="!bAC_OverrideAttenuation"))
	TObjectPtr<USoundAttenuation> AC_AttenuationSettings = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent", meta=(EditCondition="bAC_OverrideAttenuation"))
	FSoundAttenuationSettings AC_AttenuationOverrides;

	// Submix Sends
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent|Submix")
	TArray<FSoundSubmixSendInfo> AC_SubmixSends;

	// Source Effect Chain
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent|Effects")
	TObjectPtr<USoundEffectSourcePresetChain> AC_SourceEffectChain = nullptr;

	// Modulation (basic volume/pitch destinations)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent|Modulation")
	FSoundModulationDestinationSettings AC_VolumeModulation;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent|Modulation")
	FSoundModulationDestinationSettings AC_PitchModulation;

	// Concurrency
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent|Concurrency")
	TArray<TObjectPtr<USoundConcurrency>> AC_ConcurrencySet;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent|Concurrency")
	FSoundConcurrencySettings AC_ConcurrencyOverrides;

	// Auto-activate the internal audio component
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio|AudioComponent")
	bool bAC_AutoActivate = true;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void OnAudioFrame(const FString& StreamLabel, const FString& SubjectName, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate);
	void OnAudioPcm16(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes);
	bool MatchesFilter(const FString& InSubject, const FString& InStream) const;
	void EnsureSoundWave(int32 NumChannels, int32 SampleRate);

	UAudioComponent* AudioComp = nullptr;
	UPROPERTY(Transient)
	USoundWaveProcedural* SoundWave = nullptr;
	int32 CurrentChannels =0;
	int32 CurrentSampleRate =0;

	FDelegateHandle BusDelegateHandle;
};
