// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DSenderAudioCaptureComponent.h"

#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "AudioCaptureCore.h"
#include "ISubmixBufferListener.h"
#include "O3DAudioClockMapper.h"
#include "O3DAudioInputDevices.h"
#include "O3DAudioResampler.h"
#include "Transport/O3DSenderInterface.h"
#include "O3DSenderLogs.h"
#include "Misc/ScopeLock.h"

#include "HAL/PlatformTime.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

static TAutoConsoleVariable<int32> CVarO3DSenderAudioDebug(TEXT("o3ds.Sender.Audio.Debug"), 0, TEXT("Enable verbose logging for sender audio capture."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarO3DSenderAudioWarnFailures(TEXT("o3ds.Sender.Audio.WarnFailures"), 1, TEXT("Emit warnings when audio sinks reject frames."), ECVF_Default);

/**
 * Immutable capture parameters (WP-S5, SND-6). Built on the game thread from the component's
 * properties and read by the audio render and capture threads, which never touch the UObject.
 */
struct FO3DSenderAudioCaptureParams
{
    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> Sink;
    FString Label;
    float Gain = 1.0f;
    int32 OutChannels = 1;
    int32 OutSampleRate = 48000;
};

/**
 * Holds the current parameter snapshot. The game thread swaps the pointer; producers copy it.
 * The lock is held only for the pointer copy, never across processing or SubmitPcm.
 */
class FO3DSenderAudioCaptureRouter
{
public:
    using FParamsPtr = TSharedPtr<const FO3DSenderAudioCaptureParams, ESPMode::ThreadSafe>;

    void Publish(FParamsPtr InParams)
    {
        FScopeLock Lock(&Mutex);
        Params = MoveTemp(InParams);
    }

    FParamsPtr Snapshot() const
    {
        FScopeLock Lock(&Mutex);
        return Params;
    }

private:
    mutable FCriticalSection Mutex;
    FParamsPtr Params;
};

/** Per-producer scratch and log throttles (SND-6): one per submix tap, mic stream or PushFrames caller. */
struct FO3DSenderAudioProducerState
{
    /** Gain and channel mix at the input rate. */
    TArray<float> MixBuffer;
    /** Resampled output. */
    TArray<float> WorkingBuffer;
    /** Stateful per-stream resampler with anti-aliasing (SND-21). */
    FO3DAudioResampler Resampler;
    bool bResamplerInUse = false;
    /**
     * Maps the source clock (AudioClock, StreamTimeSec) onto the sender clock (ADR 0008 item 7,
     * SND-17). Unused for PushFrames, whose callers pass sender-clock times.
     */
    FO3DAudioClockMapper ClockMapper;
    double LastRejectedLogTime = 0.0;
    double LastNoSinkLogTime = 0.0;
    double LastSubmitLogTime = 0.0;
    double LastAcceptedLogTime = 0.0;
    /** Only used when several threads may share this state (PushFrames). */
    FCriticalSection Mutex;
};

namespace
{
    /** Resample/mix/gain into the producer's scratch and submit to the snapshot's sink. Any thread. */
    void ProcessAndSubmitAudio(const FO3DSenderAudioCaptureParams* Params, FO3DSenderAudioProducerState& Producer, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
    {
        if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
        {
            return;
        }

        if (!Params || !Params->Sink.IsValid())
        {
            if (CVarO3DSenderAudioDebug.GetValueOnAnyThread() != 0)
            {
                const double Now = FPlatformTime::Seconds();
                if (Now - Producer.LastNoSinkLogTime > 0.5)
                {
                    UE_LOG(LogO3DSenderAudio, Verbose, TEXT("Audio capture frame dropped (sink inactive). Frames=%d Channels=%d SampleRate=%d"),
                        NumFrames,
                        NumChannels,
                        SampleRate);
                    Producer.LastNoSinkLogTime = Now;
                }
            }
            return;
        }

        const float LocalGain = Params->Gain;
        const FString& LocalLabel = Params->Label;

        const int32 OutChannels = FMath::Max(1, Params->OutChannels);
        const int32 OutSampleRate = FMath::Max(1, Params->OutSampleRate);
        const bool bNeedsGain = !FMath::IsNearlyEqual(LocalGain, 1.0f);
        const bool bNeedsChannelMix = NumChannels != OutChannels;
        const bool bNeedsResample = SampleRate != OutSampleRate;
        const int32 InChannels = NumChannels;

        // The buffer handed to the sink. Without gain, mixing or resampling that is the
        // caller's buffer itself, with no copy.
        const float* Samples = Interleaved;

        // 1. Gain and channel mix, at the input rate.
        if (bNeedsGain || bNeedsChannelMix)
        {
            TArray<float>& MixBuffer = Producer.MixBuffer;
            MixBuffer.SetNumUninitialized(NumFrames * OutChannels, EAllowShrinking::No);
            for (int32 Frame = 0; Frame < NumFrames; ++Frame)
            {
                const float* In = Interleaved + Frame * InChannels;
                float* Out = MixBuffer.GetData() + Frame * OutChannels;
                for (int32 OutChannel = 0; OutChannel < OutChannels; ++OutChannel)
                {
                    float Sample;
                    if (OutChannels == InChannels)
                    {
                        Sample = In[OutChannel];
                    }
                    else if (OutChannels == 1)
                    {
                        float Acc = 0.0f;
                        for (int32 C = 0; C < InChannels; ++C)
                        {
                            Acc += In[C];
                        }
                        Sample = Acc / static_cast<float>(InChannels);
                    }
                    else
                    {
                        Sample = In[(InChannels == 1) ? 0 : FMath::Min(OutChannel, InChannels - 1)];
                    }
                    Out[OutChannel] = bNeedsGain ? Sample * LocalGain : Sample;
                }
            }
            Samples = MixBuffer.GetData();
            NumChannels = OutChannels;
        }

        // 2. Sample-rate conversion (SND-21): stateful across buffers, so there is no drift and
        // no click at buffer boundaries, and low-pass filtered before downsampling.
        if (bNeedsResample)
        {
            FO3DAudioResampler& Resampler = Producer.Resampler;
            Resampler.Configure(SampleRate, OutSampleRate, NumChannels);
            Producer.bResamplerInUse = true;

            double FirstFrameOffsetSec = 0.0;
            const int32 OutFrames = Resampler.Process(Samples, NumFrames, Producer.WorkingBuffer, FirstFrameOffsetSec);
            if (OutFrames <= 0)
            {
                return;
            }
            Samples = Producer.WorkingBuffer.GetData();
            NumFrames = OutFrames;
            SampleRate = OutSampleRate;
            TimestampSec += FirstFrameOffsetSec;
        }
        else if (Producer.bResamplerInUse)
        {
            // The stream stopped needing conversion; start clean if it needs it again.
            Producer.Resampler.Reset();
            Producer.bResamplerInUse = false;
        }

        const bool bDebug = CVarO3DSenderAudioDebug.GetValueOnAnyThread() != 0;
        if (bDebug)
        {
            const double Now = FPlatformTime::Seconds();
            if (Now - Producer.LastSubmitLogTime > 0.25)
            {
                UE_LOG(LogO3DSenderAudio, Log, TEXT("Submitting audio frame Label='%s' Frames=%d Channels=%d SampleRate=%d Gain=%.2f Resample=%s Mix=%s"),
                    *LocalLabel,
                    NumFrames,
                    NumChannels,
                    SampleRate,
                    LocalGain,
                    bNeedsResample ? TEXT("Yes") : TEXT("No"),
                    bNeedsChannelMix ? TEXT("Yes") : TEXT("No"));
                Producer.LastSubmitLogTime = Now;
            }
        }

        const bool bAccepted = Params->Sink->SubmitPcm(LocalLabel, Samples, NumFrames, NumChannels, SampleRate, TimestampSec);
        if (!bAccepted && CVarO3DSenderAudioWarnFailures.GetValueOnAnyThread() != 0)
        {
            const double Now = FPlatformTime::Seconds();
            if (Now - Producer.LastRejectedLogTime > 1.0)
            {
                UE_LOG(LogO3DSenderAudio, Warning, TEXT("Audio sink rejected frame (Frames=%d Channels=%d SR=%d Label=%s)"), NumFrames, NumChannels, SampleRate, *LocalLabel);
                Producer.LastRejectedLogTime = Now;
            }
        }
        else if (bAccepted && bDebug)
        {
            const double Now = FPlatformTime::Seconds();
            if (Now - Producer.LastAcceptedLogTime > 0.25)
            {
                UE_LOG(LogO3DSenderAudio, Verbose, TEXT("Audio frame accepted by transport Label='%s' Timestamp=%.3f"),
                    *LocalLabel,
                    TimestampSec);
                Producer.LastAcceptedLogTime = Now;
            }
        }
    }

    /**
     * Submix listener (runs on the audio render thread). Holds the capture router and its own
     * scratch, never the component (WP-S5, SND-6), so it needs no UObject access at all.
     */
    class FO3DSenderSubmixTap final : public ISubmixBufferListener
    {
    public:
        explicit FO3DSenderSubmixTap(TSharedRef<FO3DSenderAudioCaptureRouter, ESPMode::ThreadSafe> InRouter)
            : Router(MoveTemp(InRouter))
            , ListenerName(TEXT("O3DSenderSubmixTap"))
        {
        }

        virtual void OnNewSubmixBuffer(const USoundSubmix* /*OwningSubmix*/, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override
        {
            const int32 NumFrames = (NumChannels > 0) ? (NumSamples / NumChannels) : 0;
            if (NumFrames > 0)
            {
                // AudioClock counts from the start of audio rendering; stamp on the sender clock.
                const double TimestampSec = Producer.ClockMapper.Map(AudioClock, FPlatformTime::Seconds());
                const FO3DSenderAudioCaptureRouter::FParamsPtr Params = Router->Snapshot();
                ProcessAndSubmitAudio(Params.Get(), Producer, AudioData, NumFrames, NumChannels, SampleRate, TimestampSec);
            }
        }

        virtual const FString& GetListenerName() const override
        {
            return ListenerName;
        }

    private:
        TSharedRef<FO3DSenderAudioCaptureRouter, ESPMode::ThreadSafe> Router;
        FO3DSenderAudioProducerState Producer;
        const FString ListenerName;
    };
}

void FO3DAudioCaptureDeleter::operator()(Audio::FAudioCapture* Ptr) const
{
    delete Ptr;
}

UO3DSenderAudioCaptureComponent::UO3DSenderAudioCaptureComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    CaptureRouter = MakeShared<FO3DSenderAudioCaptureRouter, ESPMode::ThreadSafe>();
    ExternalProducer = MakeShared<FO3DSenderAudioProducerState, ESPMode::ThreadSafe>();
}

void UO3DSenderAudioCaptureComponent::OnRegister()
{
    Super::OnRegister();

    SyncConfigSourceFromMode();
}

void UO3DSenderAudioCaptureComponent::InitializeComponent()
{
    Super::InitializeComponent();
}

void UO3DSenderAudioCaptureComponent::BeginPlay()
{
    Super::BeginPlay();

    SyncConfigSourceFromMode();

    if (CVarO3DSenderAudioDebug->GetInt() != 0)
    {
        UE_LOG(LogO3DSenderAudio, Log, TEXT("Audio capture BeginPlay mode=%s sr=%d ch=%d"),
            (CaptureMode == EO3DSenderCaptureMode::Mix) ? TEXT("Mix") : TEXT("Input"),
            Config.SampleRate,
            Config.NumChannels);
    }

    RefreshCaptureParams();
    // Idempotent (SND-7): a tap registered earlier by the sender component is torn down first.
    RebuildSubmixTap();
    // ADR 0008 item 8 (SND-18): the device is opened once per start. Without a sink there is
    // nothing to capture for, and the sender component opens it in StartCaptureWithMode; a sink
    // bound later opens it in SetAudioSink.
    if (!MicCapture.IsValid() && AudioSink.IsValid())
    {
        InitializeMicCapture();
    }
}

void UO3DSenderAudioCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    TeardownSubmixTap();
    ShutdownMicCapture();

    // Drop the sink from the snapshot so no producer that is still winding down can reach it.
    if (CaptureRouter.IsValid())
    {
        CaptureRouter->Publish(nullptr);
    }

    Super::EndPlay(EndPlayReason);
}

void UO3DSenderAudioCaptureComponent::RefreshCaptureParams()
{
    if (!CaptureRouter.IsValid())
    {
        return;
    }

    if (!AudioSink.IsValid())
    {
        CaptureRouter->Publish(nullptr);
        return;
    }

    TSharedRef<FO3DSenderAudioCaptureParams, ESPMode::ThreadSafe> Params = MakeShared<FO3DSenderAudioCaptureParams, ESPMode::ThreadSafe>();
    Params->Sink = AudioSink;
    // Audio stream label is automatically derived from SubjectName
    Params->Label = SubjectName.IsEmpty() ? TEXT("o3ds:audio") : SubjectName;
    Params->Gain = (CaptureMode == EO3DSenderCaptureMode::Mix) ? Config.GameGain : Config.MicGain;
    Params->OutChannels = Config.NumChannels;
    Params->OutSampleRate = Config.SampleRate;
    CaptureRouter->Publish(Params);
}

void UO3DSenderAudioCaptureComponent::SetAudioSink(const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& InSink, const FString& InSubjectName)
{
    const bool bDebugLog = (CVarO3DSenderAudioDebug->GetInt() != 0);

    AudioSink = InSink;
    SubjectName = InSubjectName;
    RefreshCaptureParams();

    // SND-28: the submix tap runs only while a sink is bound. Before BeginPlay there is nothing
    // to stop, and BeginPlay registers the tap itself if a sink is bound by then.
    if (!AudioSink.IsValid())
    {
        TeardownSubmixTap();
    }
    else if (!SubmixTap.IsValid() && HasBegunPlay())
    {
        RebuildSubmixTap();
    }

    // A sink bound after BeginPlay opens the device if nothing has tried to since the capture was
    // last (re)started; a start whose open failed is not retried here (once per start, SND-18).
    // Opening starts the stream when a sink is bound.
    if (CaptureMode == EO3DSenderCaptureMode::Input && AudioSink.IsValid() && !MicCapture.IsValid() && !bMicOpenAttempted && HasBegunPlay())
    {
        InitializeMicCapture();
    }

    Audio::FAudioCapture* MicCaptureRaw = MicCapture.Get();
    bool bShouldStartMic = false;
    bool bShouldStopMic = false;

    if (CaptureMode == EO3DSenderCaptureMode::Input && bMicStreamOpen && MicCaptureRaw)
    {
        const bool bSinkValid = AudioSink.IsValid();
        if (bSinkValid && !bMicStreamActive)
        {
            bShouldStartMic = true;
        }
        else if (!bSinkValid && bMicStreamActive)
        {
            bShouldStopMic = true;
        }
    }

    if (bDebugLog)
    {
        UE_LOG(LogO3DSenderAudio, Log, TEXT("Audio sink %s"), AudioSink.IsValid() ? TEXT("bound") : TEXT("cleared"));
    }

    if (bShouldStartMic && MicCaptureRaw)
    {
        if (MicCaptureRaw->StartStream())
        {
            bMicStreamActive = true;
        }
        else
        {
            bMicStreamActive = false;
            UE_LOG(LogO3DSenderAudio, Warning, TEXT("Failed to start mic stream (DeviceIndex=%d)"), Config.DeviceIndex);
        }
    }
    else if (bShouldStopMic && MicCaptureRaw)
    {
        MicCaptureRaw->StopStream();
        bMicStreamActive = false;
    }
}

void UO3DSenderAudioCaptureComponent::SetStreamLabel(const FString& InSubjectName)
{
    if (SubjectName.Equals(InSubjectName, ESearchCase::CaseSensitive))
    {
        return;
    }
    SubjectName = InSubjectName;
    RefreshCaptureParams();
}

void UO3DSenderAudioCaptureComponent::StartCaptureWithMode(EO3DSenderCaptureMode InMode)
{
    // Tears down from the submix the tap was registered on, not from the (possibly new)
    // Config.SubmixToTap the caller has just written (SND-7).
    TeardownSubmixTap();

    ShutdownMicCapture();

    CaptureMode = InMode;
    SyncConfigSourceFromMode();
    RefreshCaptureParams();

    RebuildSubmixTap();
    InitializeMicCapture();

    if (CVarO3DSenderAudioDebug->GetInt() != 0)
    {
        UE_LOG(LogO3DSenderAudio, Log, TEXT("Restarted audio capture in mode %s"), (CaptureMode == EO3DSenderCaptureMode::Mix) ? TEXT("Mix") : TEXT("Input"));
    }
}

void UO3DSenderAudioCaptureComponent::SyncConfigSourceFromMode()
{
    switch (CaptureMode)
    {
    case EO3DSenderCaptureMode::Mix:
        Config.Source = EO3DSenderAudioSource::GameSubmix;
        break;
    case EO3DSenderCaptureMode::Input:
        Config.Source = EO3DSenderAudioSource::Microphone;
        break;
    default:
        break;
    }
}

void UO3DSenderAudioCaptureComponent::PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
    if (!CaptureRouter.IsValid() || !ExternalProducer.IsValid())
    {
        return;
    }

    const FO3DSenderAudioCaptureRouter::FParamsPtr Params = CaptureRouter->Snapshot();
    FScopeLock Lock(&ExternalProducer->Mutex);
    ProcessAndSubmitAudio(Params.Get(), *ExternalProducer, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}

TArray<FName> UO3DSenderAudioCaptureComponent::GetAvailableInputDeviceOptions() const
{
    // The cached list (ADR 0008 item 8): a GetOptions callback never enumerates.
    return FO3DAudioInputDevices::Get().GetNames();
}

#if WITH_EDITOR
void UO3DSenderAudioCaptureComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName Prop = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
    if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderAudioCaptureComponent, CaptureMode))
    {
        SyncConfigSourceFromMode();
    }
    else if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSenderAudioCaptureComponent, InputDeviceName))
    {
        Config.DeviceIndex = ResolveDeviceIndexFromName(InputDeviceName);
    }

    RefreshCaptureParams();
}
#endif

int32 UO3DSenderAudioCaptureComponent::ResolveDeviceIndexFromName(const FName& Name) const
{
    return FO3DAudioInputDevices::Get().FindIndex(Name);
}

void UO3DSenderAudioCaptureComponent::RebuildSubmixTap()
{
    // Idempotent (SND-7): never register a second listener while one is registered.
    TeardownSubmixTap();

    // SND-28: no tap without a sink; SetAudioSink registers it when one is bound.
    if (CaptureMode != EO3DSenderCaptureMode::Mix || !AudioSink.IsValid())
    {
        return;
    }

    if (FAudioDevice* AudioDevice = GetWorld() ? GetWorld()->GetAudioDeviceRaw() : nullptr)
    {
        Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(AudioDevice);
        if (Mixer && CaptureRouter.IsValid())
        {
            USoundSubmix* TargetSubmix = Config.SubmixToTap ? Config.SubmixToTap : &Mixer->GetMainSubmixObject();
            if (TargetSubmix)
            {
                SubmixTap = MakeShared<FO3DSenderSubmixTap, ESPMode::ThreadSafe>(CaptureRouter.ToSharedRef());
                Mixer->RegisterSubmixBufferListener(SubmixTap.ToSharedRef(), *TargetSubmix);
                TappedSubmix = TargetSubmix;
                if (CVarO3DSenderAudioDebug->GetInt() != 0)
                {
                    UE_LOG(LogO3DSenderAudio, Log, TEXT("Registered submix tap on %s"), *GetNameSafe(TargetSubmix));
                }
            }
        }
    }
}

void UO3DSenderAudioCaptureComponent::TeardownSubmixTap()
{
    if (!SubmixTap.IsValid())
    {
        TappedSubmix.Reset();
        return;
    }

    // Unregister from the submix the tap was registered on (SND-7), not Config.SubmixToTap,
    // which the sender component may already have overwritten.
    USoundSubmix* RegisteredSubmix = TappedSubmix.Get();
    FAudioDevice* AudioDevice = GetWorld() ? GetWorld()->GetAudioDeviceRaw() : nullptr;
    if (AudioDevice && RegisteredSubmix)
    {
        Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(AudioDevice);
        if (Mixer)
        {
            Mixer->UnregisterSubmixBufferListener(SubmixTap.ToSharedRef(), *RegisteredSubmix);
        }
    }
    else if (CVarO3DSenderAudioDebug->GetInt() != 0)
    {
        UE_LOG(LogO3DSenderAudio, Log, TEXT("Submix tap teardown without a live submix or audio device; the listener holds no component reference."));
    }

    SubmixTap.Reset();
    TappedSubmix.Reset();
}

void UO3DSenderAudioCaptureComponent::InitializeMicCapture()
{
    if (CaptureMode != EO3DSenderCaptureMode::Input)
    {
        return;
    }

    ++NumMicOpenAttempts;
    bMicOpenAttempted = true;
    MicCapture.Reset(new Audio::FAudioCapture());
    Audio::FAudioCaptureDeviceParams Params;
    Params.DeviceIndex = Config.DeviceIndex;

    // WP-S5 (SND-6): the capture callback holds the router and its own scratch, never `this`,
    // so it stays safe even if the device thread calls it after the component is gone.
    TSharedPtr<FO3DSenderAudioCaptureRouter, ESPMode::ThreadSafe> Router = CaptureRouter;
    TSharedPtr<FO3DSenderAudioProducerState, ESPMode::ThreadSafe> MicProducer = MakeShared<FO3DSenderAudioProducerState, ESPMode::ThreadSafe>();
    Audio::FOnAudioCaptureFunction OnCapture = [Router, MicProducer](const void* Buffer, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTimeSec, bool /*bOverflow*/)
    {
        if (!Router.IsValid())
        {
            return;
        }
        const float* PCM = reinterpret_cast<const float*>(Buffer);
        // StreamTimeSec is the device stream's own clock; stamp on the sender clock.
        const double TimestampSec = MicProducer->ClockMapper.Map(StreamTimeSec, FPlatformTime::Seconds());
        const FO3DSenderAudioCaptureRouter::FParamsPtr CaptureParams = Router->Snapshot();
        ProcessAndSubmitAudio(CaptureParams.Get(), *MicProducer, PCM, NumFrames, NumChannels, SampleRate, TimestampSec);
    };

    if (MicCapture.IsValid() && MicCapture->OpenAudioCaptureStream(Params, OnCapture, 0))
    {
        bMicStreamOpen = true;
        bMicStreamActive = false;
        StartMicCaptureIfReady();
        if (CVarO3DSenderAudioDebug->GetInt() != 0)
        {
            UE_LOG(LogO3DSenderAudio, Log, TEXT("Mic stream opened (DeviceIndex=%d)"), Config.DeviceIndex);
        }
    }
    else
    {
        UE_LOG(LogO3DSenderAudio, Warning, TEXT("Failed to open mic stream (DeviceIndex=%d)"), Config.DeviceIndex);
        MicCapture.Reset();
        bMicStreamOpen = false;
        bMicStreamActive = false;
    }
}

void UO3DSenderAudioCaptureComponent::ShutdownMicCapture()
{
    if (MicCapture.IsValid())
    {
        if (bMicStreamActive)
        {
            MicCapture->StopStream();
        }
        MicCapture->CloseStream();
        MicCapture.Reset();
    }
    bMicStreamOpen = false;
    bMicStreamActive = false;
    bMicOpenAttempted = false;
}

void UO3DSenderAudioCaptureComponent::StartMicCaptureIfReady()
{
    if (!MicCapture.IsValid() || !bMicStreamOpen || bMicStreamActive)
    {
        return;
    }

    if (AudioSink.IsValid())
    {
        if (MicCapture->StartStream())
        {
            bMicStreamActive = true;
        }
        else
        {
            bMicStreamActive = false;
            UE_LOG(LogO3DSenderAudio, Warning, TEXT("Failed to start mic stream (DeviceIndex=%d)"), Config.DeviceIndex);
        }
    }
}
