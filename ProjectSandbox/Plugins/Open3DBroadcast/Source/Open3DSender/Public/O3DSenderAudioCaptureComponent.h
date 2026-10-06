// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Sound/SoundSubmix.h"
#include "Templates/UniquePtr.h"
#include "Transport/O3DTransportTypes.h"
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
    /** Not implemented (SND-21): hidden, and never selected. Source always follows the capture mode. Kept so saved data still loads. */
    GameAndMic UMETA(Hidden, DisplayName = "Game + Mic (not implemented)")
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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (EditCondition = "false", EditConditionHides))
    EO3DSenderAudioSource Source = EO3DSenderAudioSource::GameSubmix;

    /** Submix whose output is captured in Game Submix mode. Empty: the main submix. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio")
    USoundSubmix* SubmixToTap = nullptr;

    /**
     * Sample rate sent, in Hz. Receivers play 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100 or
     * 48000; the Opus codec takes 8000, 12000, 16000, 24000 or 48000 only (SND-27). Another rate is
     * sent at the nearest of those, with a warning.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (ClampMin = "8000", ClampMax = "48000", Units = "Hz"))
    int32 SampleRate = 48000;

    /** Channels sent: 1 or 2 with the Opus codec, up to 8 with PCM. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (ClampMin = "1", ClampMax = "8"))
    int32 NumChannels = 1;

    /** Opus bitrate in kbit/s; 0 lets the encoder choose. PCM ignores it. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (ClampMin = "0", UIMax = "256"))
    int32 BitrateKbps = 64;

    /** Microphone device index in Input mode; -1 uses the default device. Set by Audio Input Device. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (ClampMin = "-1"))
    int32 DeviceIndex = -1;

    /** Gain applied to captured game (submix) audio. 1 leaves it unchanged. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (ClampMin = "0", UIMax = "4"))
    float GameGain = 1.0f;

    /** Gain applied to captured microphone audio. 1 leaves it unchanged. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio", meta = (ClampMin = "0", UIMax = "4"))
    float MicGain = 1.0f;
};

/**
 * Pure PCM audio capture component used by the broadcast sender. Captures either the master submix
 * or a microphone input and forwards frames to transport-provided sinks.
 */
UCLASS(ClassGroup = (Open3DBroadcast), meta = (BlueprintSpawnableComponent))
class OPEN3DSENDER_API UO3DSenderAudioCaptureComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UO3DSenderAudioCaptureComponent();

    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Audio")
    EO3DSenderCaptureMode CaptureMode = EO3DSenderCaptureMode::Mix;

    UPROPERTY(EditAnywhere, Category = "Open3DBroadcast|Audio", meta = (GetOptions = "GetAvailableInputDeviceOptions", EditCondition = "CaptureMode == EO3DSenderCaptureMode::Input", EditConditionHides))
    FName InputDeviceName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Audio")
    FO3DSenderAudioCaptureConfig Config;

    virtual void OnRegister() override;
    virtual void InitializeComponent() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /**
     * Bind a transport-provided audio sink. Passing nullptr disables capture delivery and stops
     * the submix tap and the microphone stream; binding a sink starts them again (SND-28).
     * Game thread.
     */
    void SetAudioSink(const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& InSink, const FString& InSubjectName);

    /**
     * Change the audio stream label (the pose subject name, SND-16) without rebinding the sink.
     * Game thread.
     */
    void SetStreamLabel(const FString& InSubjectName);

    /** Change capture mode and immediately restart capture resources. Game thread. */
    void StartCaptureWithMode(EO3DSenderCaptureMode InMode);

    /**
     * Republish the immutable parameter snapshot (sink, label, gain, target format) that the
     * audio and capture threads read (WP-S5, SND-6). Call on the game thread after changing
     * Config or CaptureMode directly. SetAudioSink and StartCaptureWithMode call it.
     */
    void RefreshCaptureParams();

    /**
     * Forward PCM frames as if captured. Any thread; never touches this UObject's properties.
     * TimestampSec is passed through unchanged, so give it on the sender clock
     * (FPlatformTime::Seconds(), ADR 0009 item 7); the submix tap and the microphone map their own
     * clocks onto it.
     */
    void PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);

    /** The cached capture device names (ADR 0008 item 8); never enumerates. */
    UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Audio")
    TArray<FName> GetAvailableInputDeviceOptions() const;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
    friend struct FO3DSenderAudioCaptureTestAccess;

    /** The cached index of a device name (ADR 0008 item 8); never enumerates. */
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
    /** InitializeMicCapture ran since the last ShutdownMicCapture, whether or not the open succeeded. */
    bool bMicOpenAttempted = false;
    /** Times InitializeMicCapture tried to open the device (tests: once per start, SND-18). */
    int32 NumMicOpenAttempts = 0;
};
