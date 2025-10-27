#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DSRemoteAudioComponent.generated.h"

class USoundWaveProcedural;
class UAudioComponent;
class FWebRTCConnector;

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DSTREAM_API UO3DSRemoteAudioComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UO3DSRemoteAudioComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	FName SubjectName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	float Volume =1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	bool bAutoPlay = true;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void OnRemotePcm(const FString& Subject, const FString& Stream, const int16* PCM, int32 NumSamples, int32 NumChannels);

	void EnsureAudioObjects(int32 NumChannels, int32 SampleRate);

	// Audio objects
	UPROPERTY(Transient)
	UAudioComponent* AudioComponent = nullptr;
	UPROPERTY(Transient)
	USoundWaveProcedural* ProcWave = nullptr;

	TWeakPtr<FWebRTCConnector> ConnectorRef;
	FDelegateHandle DelegateHandle;
};
