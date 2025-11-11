#include "O3DSenderAudioCaptureComponent.h"

#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "AudioCaptureCore.h"
#include "ISubmixBufferListener.h"
#include "O3DSenderInterface.h"
#include "O3DSenderLogs.h"
#include "Misc/ScopeLock.h"

#include "HAL/PlatformTime.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
    class FO3DSenderSubmixTap final : public ISubmixBufferListener
    {
    public:
        explicit FO3DSenderSubmixTap(UO3DSenderAudioCaptureComponent* InOwner)
            : Owner(InOwner)
        {
        }

        virtual void OnNewSubmixBuffer(const USoundSubmix* /*OwningSubmix*/, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override
        {
            if (UO3DSenderAudioCaptureComponent* StrongOwner = Owner.Get())
            {
                const int32 NumFrames = (NumChannels > 0) ? (NumSamples / NumChannels) : 0;
                if (NumFrames > 0)
                {
                    StrongOwner->PushFrames(AudioData, NumFrames, NumChannels, SampleRate, AudioClock);
                }
            }
        }

    private:
        TWeakObjectPtr<UO3DSenderAudioCaptureComponent> Owner;
    };
}

void FO3DAudioCaptureDeleter::operator()(Audio::FAudioCapture* Ptr) const
{
    delete Ptr;
}

static TAutoConsoleVariable<int32> CVarO3DSenderAudioDebug(TEXT("o3ds.Sender.Audio.Debug"), 0, TEXT("Enable verbose logging for sender audio capture."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarO3DSenderAudioWarnFailures(TEXT("o3ds.Sender.Audio.WarnFailures"), 1, TEXT("Emit warnings when audio sinks reject frames."), ECVF_Default);

UO3DSenderAudioCaptureComponent::UO3DSenderAudioCaptureComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
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

    RebuildSubmixTap();
    InitializeMicCapture();
}

void UO3DSenderAudioCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    TeardownSubmixTap();
    ShutdownMicCapture();

    Super::EndPlay(EndPlayReason);
}

void UO3DSenderAudioCaptureComponent::SetStreamLabel(const FString& InLabel)
{
    FScopeLock Lock(&SinkMutex);
    StreamLabel = InLabel;
}

void UO3DSenderAudioCaptureComponent::SetAudioSink(const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& InSink, const FO3DTransportAudioConfig& InAudioConfig)
{
    const bool bDebugLog = (CVarO3DSenderAudioDebug->GetInt() != 0);

    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> LocalSink;
    Audio::FAudioCapture* MicCaptureRaw = nullptr;
    bool bShouldStartMic = false;
    bool bShouldStopMic = false;

    {
        FScopeLock Lock(&SinkMutex);
        AudioSink = InSink;
        ActiveAudioConfig = InAudioConfig;
        if (ActiveAudioConfig.StreamLabel.IsEmpty())
        {
            ActiveAudioConfig.StreamLabel = StreamLabel.IsEmpty() ? TEXT("o3ds:audio") : StreamLabel;
        }
        else
        {
            StreamLabel = ActiveAudioConfig.StreamLabel;
        }

        LocalSink = AudioSink;
        MicCaptureRaw = MicCapture.Get();

        if (CaptureMode == EO3DSenderCaptureMode::Input && bMicStreamOpen && MicCaptureRaw)
        {
            const bool bSinkValid = LocalSink.IsValid();
            if (bSinkValid && !bMicStreamActive)
            {
                bShouldStartMic = true;
            }
            else if (!bSinkValid && bMicStreamActive)
            {
                bShouldStopMic = true;
            }
        }
    }

    if (bDebugLog)
    {
        UE_LOG(LogO3DSenderAudio, Log, TEXT("Audio sink %s"), LocalSink.IsValid() ? TEXT("bound") : TEXT("cleared"));
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

void UO3DSenderAudioCaptureComponent::StartCaptureWithMode(EO3DSenderCaptureMode InMode)
{
    TeardownSubmixTap();

    ShutdownMicCapture();

    CaptureMode = InMode;
    SyncConfigSourceFromMode();

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
    ProcessAndSubmitAudio(Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}

TArray<FName> UO3DSenderAudioCaptureComponent::GetAvailableInputDeviceOptions() const
{
    TArray<FName> Options;
    Audio::FAudioCapture Temp;
    TArray<Audio::FCaptureDeviceInfo> Devices;
    if (Temp.GetCaptureDevicesAvailable(Devices) > 0)
    {
        for (const Audio::FCaptureDeviceInfo& Info : Devices)
        {
            Options.Add(FName(*Info.DeviceName));
        }
    }
    return Options;
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
}
#endif

int32 UO3DSenderAudioCaptureComponent::ResolveDeviceIndexFromName(const FName& Name) const
{
    if (Name.IsNone())
    {
        return -1;
    }

    Audio::FAudioCapture Temp;
    TArray<Audio::FCaptureDeviceInfo> Devices;
    if (Temp.GetCaptureDevicesAvailable(Devices) > 0)
    {
        for (int32 Index = 0; Index < Devices.Num(); ++Index)
        {
            if (Devices[Index].DeviceName.Equals(Name.ToString(), ESearchCase::IgnoreCase))
            {
                return Index;
            }
        }
    }
    return -1;
}

void UO3DSenderAudioCaptureComponent::RebuildSubmixTap()
{
    if (CaptureMode != EO3DSenderCaptureMode::Mix)
    {
        return;
    }

    if (FAudioDevice* AudioDevice = GetWorld() ? GetWorld()->GetAudioDeviceRaw() : nullptr)
    {
        Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(AudioDevice);
        if (Mixer)
        {
            if (!SubmixTap.IsValid())
            {
                SubmixTap = MakeShared<FO3DSenderSubmixTap, ESPMode::ThreadSafe>(this);
            }

            USoundSubmix* TargetSubmix = Config.SubmixToTap ? Config.SubmixToTap : &Mixer->GetMainSubmixObject();
            if (TargetSubmix)
            {
                Mixer->RegisterSubmixBufferListener(SubmixTap.ToSharedRef(), *TargetSubmix);
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
        return;
    }

    if (FAudioDevice* AudioDevice = GetWorld() ? GetWorld()->GetAudioDeviceRaw() : nullptr)
    {
        Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(AudioDevice);
        if (Mixer)
        {
            USoundSubmix* TargetSubmix = Config.SubmixToTap ? Config.SubmixToTap : &Mixer->GetMainSubmixObject();
            if (TargetSubmix)
            {
                Mixer->UnregisterSubmixBufferListener(SubmixTap.ToSharedRef(), *TargetSubmix);
            }
        }
    }

    SubmixTap.Reset();
}

void UO3DSenderAudioCaptureComponent::InitializeMicCapture()
{
    if (CaptureMode != EO3DSenderCaptureMode::Input)
    {
        return;
    }

    MicCapture.Reset(new Audio::FAudioCapture());
    Audio::FAudioCaptureDeviceParams Params;
    Params.DeviceIndex = Config.DeviceIndex;

    Audio::FOnAudioCaptureFunction OnCapture = [this](const void* Buffer, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTimeSec, bool /*bOverflow*/)
    {
        const float* PCM = reinterpret_cast<const float*>(Buffer);
        PushFrames(PCM, NumFrames, NumChannels, SampleRate, StreamTimeSec);
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
}

void UO3DSenderAudioCaptureComponent::StartMicCaptureIfReady()
{
    if (!MicCapture.IsValid() || !bMicStreamOpen || bMicStreamActive)
    {
        return;
    }

    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> LocalSink;
    {
        FScopeLock Lock(&SinkMutex);
        LocalSink = AudioSink;
    }

    if (LocalSink.IsValid())
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

void UO3DSenderAudioCaptureComponent::ProcessAndSubmitAudio(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
    if (!Interleaved || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
    {
        return;
    }

    TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> LocalSink;
    FO3DTransportAudioConfig LocalConfig;
    float LocalGain = 1.0f;
    FString LocalLabel;
    {
        FScopeLock Lock(&SinkMutex);
        LocalSink = AudioSink;
        LocalConfig = ActiveAudioConfig;
        LocalLabel = LocalConfig.StreamLabel.IsEmpty() ? StreamLabel : LocalConfig.StreamLabel;
        LocalGain = (CaptureMode == EO3DSenderCaptureMode::Mix) ? Config.GameGain : Config.MicGain;
    }

    if (!LocalSink.IsValid() || !LocalConfig.bEnableAudio)
    {
        if (CVarO3DSenderAudioDebug->GetInt() != 0)
        {
            static double LastNoSinkLogTime = 0.0;
            const double Now = FPlatformTime::Seconds();
            if (Now - LastNoSinkLogTime > 0.5)
            {
                UE_LOG(LogO3DSenderAudio, Verbose, TEXT("Audio capture frame dropped (sink inactive). Frames=%d Channels=%d SampleRate=%d Label='%s'"),
                    NumFrames,
                    NumChannels,
                    SampleRate,
                    *LocalLabel);
                LastNoSinkLogTime = Now;
            }
        }
        return;
    }

    const int32 OutChannels = FMath::Max(1, LocalConfig.NumChannels);
    const int32 OutSampleRate = FMath::Max(1, LocalConfig.SampleRate);
    const bool bNeedsGain = !FMath::IsNearlyEqual(LocalGain, 1.0f);
    const bool bNeedsChannelMix = NumChannels != OutChannels;
    const bool bNeedsResample = SampleRate != OutSampleRate;

    if (!bNeedsGain && !bNeedsChannelMix && !bNeedsResample)
    {
        WorkingBuffer.SetNumUninitialized(NumFrames * NumChannels);
        FMemory::Memcpy(WorkingBuffer.GetData(), Interleaved, WorkingBuffer.Num() * sizeof(float));
    }
    else
    {
        const double Ratio = static_cast<double>(OutSampleRate) / static_cast<double>(SampleRate);
        const int32 OutFrames = bNeedsResample ? FMath::Max(1, static_cast<int32>(FMath::RoundToDouble(NumFrames * Ratio))) : NumFrames;
        WorkingBuffer.SetNumUninitialized(OutFrames * OutChannels);

        auto SampleAt = [&](double FrameIndex, int32 Channel) -> float
        {
            const double SrcPos = FrameIndex;
            const int32 Index0 = FMath::Clamp(static_cast<int32>(FMath::FloorToDouble(SrcPos)), 0, NumFrames - 1);
            const int32 Index1 = FMath::Clamp(Index0 + 1, 0, NumFrames - 1);
            const float Alpha = static_cast<float>(SrcPos - static_cast<double>(Index0));
            const int32 SrcChannel = FMath::Clamp(Channel, 0, NumChannels - 1);
            const int32 Base0 = Index0 * NumChannels + SrcChannel;
            const int32 Base1 = Index1 * NumChannels + SrcChannel;
            const float S0 = Interleaved[Base0];
            const float S1 = Interleaved[Base1];
            return FMath::Lerp(S0, S1, Alpha);
        };

        for (int32 OutFrame = 0; OutFrame < OutFrames; ++OutFrame)
        {
            const double SrcFrame = bNeedsResample ? (static_cast<double>(OutFrame) / Ratio) : static_cast<double>(OutFrame);
            for (int32 OutChannel = 0; OutChannel < OutChannels; ++OutChannel)
            {
                float Sample;
                if (OutChannels == NumChannels)
                {
                    Sample = SampleAt(SrcFrame, OutChannel);
                }
                else if (OutChannels == 1)
                {
                    float Acc = 0.0f;
                    for (int32 C = 0; C < NumChannels; ++C)
                    {
                        Acc += SampleAt(SrcFrame, C);
                    }
                    Sample = Acc / static_cast<float>(NumChannels);
                }
                else
                {
                    const int32 SourceChannel = (NumChannels == 1) ? 0 : FMath::Min(OutChannel, NumChannels - 1);
                    Sample = SampleAt(SrcFrame, SourceChannel);
                }

                if (bNeedsGain)
                {
                    Sample *= LocalGain;
                }

                WorkingBuffer[OutFrame * OutChannels + OutChannel] = Sample;
            }
        }

        NumFrames = OutFrames;
        NumChannels = OutChannels;
        SampleRate = OutSampleRate;
    }

    if (!bNeedsGain && (bNeedsChannelMix || bNeedsResample))
    {
        // When only resampling/channel mixing was required but no gain, values were written in loop above.
    }
    else if ((!bNeedsChannelMix && !bNeedsResample) && bNeedsGain)
    {
        for (float& Value : WorkingBuffer)
        {
            Value *= LocalGain;
        }
    }

    if (CVarO3DSenderAudioDebug->GetInt() != 0)
    {
        static double LastSubmitLogTime = 0.0;
        const double Now = FPlatformTime::Seconds();
        if (Now - LastSubmitLogTime > 0.25)
        {
            UE_LOG(LogO3DSenderAudio, Log, TEXT("Submitting audio frame Label='%s' Frames=%d Channels=%d SampleRate=%d Gain=%.2f Resample=%s Mix=%s"),
                *LocalLabel,
                NumFrames,
                NumChannels,
                SampleRate,
                LocalGain,
                bNeedsResample ? TEXT("Yes") : TEXT("No"),
                bNeedsChannelMix ? TEXT("Yes") : TEXT("No"));
            LastSubmitLogTime = Now;
        }
    }

    const bool bAccepted = LocalSink->SubmitPcm(LocalLabel, WorkingBuffer.GetData(), NumFrames, NumChannels, SampleRate, TimestampSec);
    if (!bAccepted && CVarO3DSenderAudioWarnFailures->GetInt() != 0)
    {
        const double Now = FPlatformTime::Seconds();
        if (Now - LastRejectedLogTime > 1.0)
        {
            UE_LOG(LogO3DSenderAudio, Warning, TEXT("Audio sink rejected frame (Frames=%d Channels=%d SR=%d Label=%s)"), NumFrames, NumChannels, SampleRate, *LocalLabel);
            LastRejectedLogTime = Now;
        }
    }
    else if (bAccepted && CVarO3DSenderAudioDebug->GetInt() != 0)
    {
        static double LastAcceptedLogTime = 0.0;
        const double Now = FPlatformTime::Seconds();
        if (Now - LastAcceptedLogTime > 0.25)
        {
            UE_LOG(LogO3DSenderAudio, Verbose, TEXT("Audio frame accepted by transport Label='%s' Timestamp=%.3f"),
                *LocalLabel,
                TimestampSec);
            LastAcceptedLogTime = Now;
        }
    }
}
