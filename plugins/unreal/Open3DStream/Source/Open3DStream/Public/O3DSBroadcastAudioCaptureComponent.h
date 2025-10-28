#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Sound/SoundSubmix.h"
#include "O3DSBroadcastAudioCaptureComponent.generated.h" // <-- This is required for GENERATED_BODY and UPROPERTY

class IWebRTCConnector;
class ISubmixBufferListener;

namespace Audio { class FAudioCapture; }

UENUM(BlueprintType)
enum class EO3DSAudioCaptureSource : uint8
{
	GameSubmix UMETA(DisplayName="Game Submix"),
	Microphone UMETA(DisplayName="Microphone"),
	GameAndMic UMETA(DisplayName="Game + Mic")
};

UENUM(BlueprintType)
enum class EO3DSCaptureMode : uint8
{
	Mix UMETA(DisplayName="Mix (Main Submix or Custom)"),
	Input UMETA(DisplayName="Input (Microphone)")
};

USTRUCT(BlueprintType)
struct FO3DSAudioCaptureConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
	EO3DSAudioCaptureSource Source = EO3DSAudioCaptureSource::GameSubmix;

	// No EditCondition here; gated UX is provided by component-level CaptureMode
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
	USoundSubmix* SubmixToTap = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
	int32 SampleRate = 48000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio"	)
	int32 NumChannels = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
	int32 BitrateKbps = 64;

	// Optional device index when using Input mode; -1 uses system default
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio", meta = (ClampMin = "-1"))
	int32 DeviceIndex = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
	float GameGain = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
	float MicGain = 1.0f;
};

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DSTREAM_API UO3DSBroadcastAudioCaptureComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UO3DSBroadcastAudioCaptureComponent();

	// High-level UX: choose Mix vs Input. This drives Config.Source at runtime.
	UPROPERTY(EditAnywhere, Category="Open3DStream|Audio")
	EO3DSCaptureMode CaptureMode = EO3DSCaptureMode::Mix;

	// Device name dropdown (only when Input), feeds Config.DeviceIndex internally
	UPROPERTY(EditAnywhere, Category="Open3DStream|Audio", meta=(GetOptions="GetAvailableInputDeviceOptions", EditCondition="CaptureMode == EO3DSCaptureMode::Input", EditConditionHides))
	FName InputDeviceName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	FO3DSAudioCaptureConfig Config;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Audio")
	FName SubjectName;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// Public so submix tap can invoke it
	void PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);

	// Options provider for the device dropdown
	UFUNCTION()
	TArray<FName> GetAvailableInputDeviceOptions() const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void EnsureConnector();
	int32 ResolveDeviceIndexFromName(const FName& Name) const;

	TSharedPtr<IWebRTCConnector> Connector;
	FString StreamLabel;

	// Submix tap listener and optional microphone capture
	TSharedPtr<ISubmixBufferListener, ESPMode::ThreadSafe> SubmixTap;
	Audio::FAudioCapture* MicCapture = nullptr;
};
