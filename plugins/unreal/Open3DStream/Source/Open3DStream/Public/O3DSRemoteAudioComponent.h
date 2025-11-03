// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LiveLinkTypes.h"
#include "O3DSRemoteAudioComponent.generated.h"

class UAudioComponent;
class USoundWaveProcedural;

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

	// Auto-create and attach an AudioComponent to owner
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	bool bAutoCreateAudioComponent = true;

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
	// True if we created the AudioComponent; in that case we own its VolumeMultiplier (apply Gain)
	bool bOwnsAudioComponent = false;

	FDelegateHandle BusDelegateHandle;
};
