#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Sound/SoundSubmix.h"
#include "O3DSBroadcastAudioCaptureComponent.generated.h"

class IWebRTCConnector;

UENUM(BlueprintType)
enum class EO3DSAudioCaptureSource : uint8
{
	GameSubmix UMETA(DisplayName="Game Submix"),
	Microphone UMETA(DisplayName="Microphone"),
	GameAndMic UMETA(DisplayName="Game + Mic")
};

USTRUCT(BlueprintType)
struct FO3DSAudioCaptureConfig
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EO3DSAudioCaptureSource Source = EO3DSAudioCaptureSource::GameSubmix;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	USoundSubmix* SubmixToTap = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 SampleRate =48000;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 NumChannels =1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 BitrateKbps =64;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float GameGain =1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float MicGain =1.0f;
};

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DSTREAM_API UO3DSBroadcastAudioCaptureComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UO3DSBroadcastAudioCaptureComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	FO3DSAudioCaptureConfig Config;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	FName SubjectName;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void EnsureConnector();
	void PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);

	TSharedPtr<IWebRTCConnector> Connector;
	FString StreamLabel;
};
