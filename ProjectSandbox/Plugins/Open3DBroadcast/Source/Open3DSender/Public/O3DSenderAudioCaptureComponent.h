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
class FO3DSenderAudioCaptureRouter;
struct FO3DSenderAudioProducerState;

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

    /** Bind a transport-provided audio sink. Passing nullptr disables capture delivery. Game thread. */
    void SetAudioSink(const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& InSink, const FString& InSubjectName);

    /** Change capture mode and immediately restart capture resources. Game thread. */
    void StartCaptureWithMode(EO3DSenderCaptureMode InMode);

    /**
     * Republish the immutable parameter snapshot (sink, label, gain, target format) that the
     * audio and capture threads read (WP-S5, SND-6). Call on the game thread after changing
     * Config or CaptureMode directly. SetAudioSink and StartCaptureWithMode call it.
     */
    void RefreshCaptureParams();

    /** Forward PCM frames as if captured. Any thread; never touches this UObject's properties. */
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

    // Game-thread state. Audio and capture threads never read these (SND-6): they read the
    // immutable snapshot published through CaptureRouter.
    FString SubjectName;  // Audio stream label is derived from this
    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> AudioSink;

    /** Holds the current parameter snapshot; shared with the submix tap and the mic callback. */
    TSharedPtr<FO3DSenderAudioCaptureRouter, ESPMode::ThreadSafe> CaptureRouter;

    /** Scratch for PushFrames callers (per-producer scratch, SND-6); guarded by its own lock. */
    TSharedPtr<FO3DSenderAudioProducerState, ESPMode::ThreadSafe> ExternalProducer;

    TSharedPtr<ISubmixBufferListener, ESPMode::ThreadSafe> SubmixTap;
    /** The submix the tap was registered on; always the one it is unregistered from (SND-7). */
    TWeakObjectPtr<USoundSubmix> TappedSubmix;
    TUniquePtr<Audio::FAudioCapture, FO3DAudioCaptureDeleter> MicCapture;

    bool bMicStreamOpen = false;
    bool bMicStreamActive = false;
};
