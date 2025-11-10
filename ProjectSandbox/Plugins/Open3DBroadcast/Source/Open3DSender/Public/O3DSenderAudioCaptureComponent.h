#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Sound/SoundSubmix.h"
#include "Templates/UniquePtr.h"
#include "O3DTransportTypes.h"
#include "O3DSenderAudioCaptureComponent.generated.h"

class ISubmixBufferListener;
namespace Audio
{
    class FAudioCapture;
}
class IO3DSenderAudioSink;

struct FO3DAudioCaptureDeleter
{
    void operator()(Audio::FAudioCapture* Ptr) const;
};

UENUM(BlueprintType)
enum class EO3DSenderAudioSource : uint8
{
    GameSubmix UMETA(DisplayName = "Game Submix"),
    Microphone UMETA(DisplayName = "Microphone"),
    GameAndMic UMETA(DisplayName = "Game + Mic")
};

UENUM(BlueprintType)
enum class EO3DSenderCaptureMode : uint8
{
    Mix UMETA(DisplayName = "Mix (Main Submix or Custom)"),
    Input UMETA(DisplayName = "Input (Microphone)")
};

USTRUCT(BlueprintType)
struct FO3DSenderAudioCaptureConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio", meta = (EditCondition = "false", EditConditionHides))
    EO3DSenderAudioSource Source = EO3DSenderAudioSource::GameSubmix;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    USoundSubmix* SubmixToTap = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    int32 SampleRate = 48000;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    int32 NumChannels = 1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    int32 BitrateKbps = 64;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio", meta = (ClampMin = "-1"))
    int32 DeviceIndex = -1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    float GameGain = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    float MicGain = 1.0f;
};

/**
 * Pure PCM audio capture component used by the broadcast sender. Captures either the master submix
 * or a microphone input and forwards frames to transport-provided sinks.
 */
UCLASS(ClassGroup = (Open3DStream), meta = (BlueprintSpawnableComponent))
class OPEN3DSENDER_API UO3DSenderAudioCaptureComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UO3DSenderAudioCaptureComponent();

    UPROPERTY(EditAnywhere, Category = "Open3DStream|Audio")
    EO3DSenderCaptureMode CaptureMode = EO3DSenderCaptureMode::Mix;

    UPROPERTY(EditAnywhere, Category = "Open3DStream|Audio", meta = (GetOptions = "GetAvailableInputDeviceOptions", EditCondition = "CaptureMode == EO3DSenderCaptureMode::Input", EditConditionHides))
    FName InputDeviceName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DStream|Audio")
    FO3DSenderAudioCaptureConfig Config;

    virtual void OnRegister() override;
    virtual void InitializeComponent() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** Update the user-facing stream label, mainly for debugging. */
    void SetStreamLabel(const FString& InLabel);

    /** Bind a transport-provided audio sink. Passing nullptr disables capture delivery. */
    void SetAudioSink(const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& InSink, const FO3DTransportAudioConfig& InAudioConfig);

    /** Change capture mode and immediately restart capture resources. */
    void StartCaptureWithMode(EO3DSenderCaptureMode InMode);

    /** Forward PCM frames from submix/mic capture. */
    void PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);

    UFUNCTION(BlueprintCallable, Category = "Open3DStream|Audio")
    TArray<FName> GetAvailableInputDeviceOptions() const;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    int32 ResolveDeviceIndexFromName(const FName& Name) const;
    void RebuildSubmixTap();
    void TeardownSubmixTap();
    void InitializeMicCapture();
    void ShutdownMicCapture();
    void StartMicCaptureIfReady();
    void SyncConfigSourceFromMode();

    void ProcessAndSubmitAudio(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);

    FString StreamLabel;

    mutable FCriticalSection SinkMutex;
    FO3DTransportAudioConfig ActiveAudioConfig;
    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink;

    TSharedPtr<ISubmixBufferListener, ESPMode::ThreadSafe> SubmixTap;
    TUniquePtr<Audio::FAudioCapture, FO3DAudioCaptureDeleter> MicCapture;

    TArray<float> WorkingBuffer;

    double LastRejectedLogTime = 0.0;

    bool bMicStreamOpen = false;
    bool bMicStreamActive = false;
};
