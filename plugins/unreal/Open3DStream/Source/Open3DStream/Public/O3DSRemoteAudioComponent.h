// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DSAudioBus.h"
#include "O3DSRemoteAudioComponent.generated.h"

class USoundWaveProcedural;
class UAudioComponent;

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DSTREAM_API UO3DSRemoteAudioComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UO3DSRemoteAudioComponent();

    // Filter: only play streams whose label matches. Empty means accept all.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="O3DS|Audio")
    FString StreamLabelFilter;

    // Optional subject filter (extracted from label if present). Empty accepts all.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="O3DS|Audio")
    FString SubjectNameFilter;

    // Auto-create and attach an AudioComponent on begin play if not present
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="O3DS|Audio")
    bool bAutoCreateAudioComponent = true;

    // Gain multiplier applied to queued PCM
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="O3DS|Audio", meta=(ClampMin="0.0", ClampMax="4.0"))
    float Gain = 1.0f;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    void OnAudioFrame(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes);
    bool MatchesFilter(const O3DS::FAudioFrameMeta& Meta) const;
    void EnsureSoundWave(int32 NumChannels, int32 SampleRate);

private:
    FDelegateHandle BusHandle;
    UPROPERTY(Transient)
    USoundWaveProcedural* SoundWave = nullptr;
    UPROPERTY(Transient)
    UAudioComponent* AudioComp = nullptr;
    int32 CurrentChannels = 0;
    int32 CurrentSampleRate = 0;
};
