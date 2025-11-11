// Copyright (c) Open3DStream Contributors

#include "O3DRemoteAudioComponent.h"

#include "O3DAudioBus.h"
#include "O3DReceiverLogs.h"
#include "O3DUnifiedMessage.h"

#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundSubmixSend.h"
#include "Sound/SoundWaveProcedural.h"

static TAutoConsoleVariable<int32> CVarO3DSRemoteAudioDebug(
    TEXT("o3ds.RemoteAudio.Debug"),
    0,
    TEXT("Enable debug logs for O3DS remote audio component (0/1)."),
    ECVF_Default);

UO3DRemoteAudioComponent::UO3DRemoteAudioComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UO3DRemoteAudioComponent::OnRegister()
{
    Super::OnRegister();
    AttachToConfiguredParent();
}

void UO3DRemoteAudioComponent::AttachToConfiguredParent()
{
    USceneComponent* ParentToAttach = nullptr;
    if (AActor* Owner = GetOwner())
    {
        if (UActorComponent* RefComp = AC_AttachParent.GetComponent(Owner))
        {
            ParentToAttach = Cast<USceneComponent>(RefComp);
        }
        if (!ParentToAttach)
        {
            ParentToAttach = Owner->GetRootComponent();
        }
    }

    if (ParentToAttach && ParentToAttach != GetAttachParent())
    {
        AttachToComponent(ParentToAttach, FAttachmentTransformRules::KeepRelativeTransform, AC_AttachSocketName);
    }
}

void UO3DRemoteAudioComponent::BeginPlay()
{
    Super::BeginPlay();

    AudioComp = NewObject<UAudioComponent>(GetOwner());
    if (AudioComp)
    {
        AudioComp->RegisterComponent();
        AudioComp->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
        AudioComp->bAutoActivate = bAC_AutoActivate;
        AudioComp->bAllowSpatialization = bAC_AllowSpatialization;
        AudioComp->bIsUISound = bAC_IsUISound;
        AudioComp->bOverrideAttenuation = bAC_OverrideAttenuation;
        AudioComp->AttenuationSettings = AC_AttenuationSettings;
        AudioComp->AttenuationOverrides = AC_AttenuationOverrides;
        AudioComp->SetPitchMultiplier(AC_PitchMultiplier);
        AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, AC_VolumeMultiplier * Gain));
        bOwnsAudioComponent = true;
    }

    BusDelegateHandle = FO3DAudioBus::OnPcm16().AddUObject(this, &UO3DRemoteAudioComponent::OnAudioPcm16);

    static bool bOnce = false;
    if (!bOnce)
    {
        bOnce = true;
        UE_LOG(LogO3DReceiverAudio, Log, TEXT("Subscribed to FO3DAudioBus (Gain=%.2f)"), Gain);
        if (!AudioComp)
        {
            UE_LOG(LogO3DReceiverAudio, Warning, TEXT("No UAudioComponent present/created on owner '%s'"), *GetNameSafe(GetOwner()));
        }
    }

    if (CVarO3DSRemoteAudioDebug->GetInt() != 0)
    {
        UE_LOG(LogO3DReceiverAudio, Log, TEXT("Remote audio debug enabled"));
    }
}

void UO3DRemoteAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (BusDelegateHandle.IsValid())
    {
        FO3DAudioBus::OnPcm16().Remove(BusDelegateHandle);
        BusDelegateHandle.Reset();
    }

    Super::EndPlay(EndPlayReason);
}

bool UO3DRemoteAudioComponent::MatchesFilter(const FString& InSubject, const FString& InStream) const
{
    const bool bIsMix = InStream.StartsWith(TEXT("o3ds:mix"));
    bool bSubjectMatch = false;
    if (ReceiveMode == EO3DRemoteAudioMode::Mix)
    {
        bSubjectMatch = bIsMix;
    }
    else
    {
        const FString Desired = LiveLinkSubjectName.Name.ToString();
        bSubjectMatch = !Desired.IsEmpty() && InSubject.Equals(Desired, ESearchCase::IgnoreCase);
    }

    if (!bSubjectMatch)
    {
        return false;
    }

    if (!StreamLabelFilter.IsEmpty() && !InStream.Contains(StreamLabelFilter))
    {
        return false;
    }

    if (CVarO3DSRemoteAudioDebug->GetInt() != 0)
    {
        UE_LOG(LogO3DReceiverAudio, Verbose, TEXT("Filter pass subject='%s' stream='%s'"), *InSubject, *InStream);
    }

    return true;
}

void UO3DRemoteAudioComponent::EnsureSoundWave(int32 NumChannels, int32 SampleRate)
{
    const bool bNeedNew = (SoundWave == nullptr) || (CurrentChannels != NumChannels) || (CurrentSampleRate != SampleRate);
    if (bNeedNew)
    {
        SoundWave = NewObject<USoundWaveProcedural>(this);
        if (SoundWave)
        {
            SoundWave->bLooping = false;
            SoundWave->NumChannels = NumChannels;
            SoundWave->SetSampleRate(SampleRate);
            SoundWave->Duration = INDEFINITELY_LOOPING_DURATION;
            SoundWave->SoundGroup = SOUNDGROUP_Voice;
            SoundWave->bProcedural = true;
            SoundWave->SourceEffectChain = AC_SourceEffectChain;
            SoundWave->SoundSubmixSends = AC_SubmixSends;
            SoundWave->ConcurrencyOverrides = AC_ConcurrencyOverrides;
            SoundWave->ConcurrencySet.Reset();
            for (TObjectPtr<USoundConcurrency> Concurrency : AC_ConcurrencySet)
            {
                if (Concurrency)
                {
                    SoundWave->ConcurrencySet.Add(Concurrency);
                }
            }
        }
        CurrentChannels = NumChannels;
        CurrentSampleRate = SampleRate;

        if (AudioComp)
        {
            AudioComp->SetSound(SoundWave);
            if (bAC_AutoActivate && !AudioComp->IsPlaying())
            {
                AudioComp->Play();
            }
            AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, AC_VolumeMultiplier * Gain));

            if (CVarO3DSRemoteAudioDebug->GetInt() != 0)
            {
                UE_LOG(LogO3DReceiverAudio, Log, TEXT("SoundWave prepared ch=%d sr=%d; AudioComp playing=%d"),
                    NumChannels,
                    SampleRate,
                    AudioComp->IsPlaying() ? 1 : 0);
            }
        }
    }
}

void UO3DRemoteAudioComponent::OnAudioFrame(const FString& StreamLabel, const FString& SubjectName, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate)
{
    if (!MatchesFilter(SubjectName, StreamLabel))
    {
        if (CVarO3DSRemoteAudioDebug->GetInt() != 0)
        {
            static int32 DropEvery = 0;
            if ((DropEvery++ % 100) == 0)
            {
                UE_LOG(LogO3DReceiverAudio, Verbose, TEXT("Dropped frame by filter subject='%s' stream='%s'"), *SubjectName, *StreamLabel);
            }
        }
        return;
    }

    EnsureSoundWave(NumChannels, SampleRate);
    if (!SoundWave || NumFrames <= 0 || NumChannels <= 0)
    {
        return;
    }

    const int32 NumSamples = NumFrames * NumChannels;
    TArray<int16> PCM16;
    PCM16.AddUninitialized(NumSamples);
    for (int32 i = 0; i < NumSamples; ++i)
    {
        float Value = Interleaved[i] * Gain;
        Value = FMath::Clamp(Value, -1.0f, 1.0f);
        PCM16[i] = static_cast<int16>(FMath::RoundToInt(Value * 32767.0f));
    }

    SoundWave->QueueAudio(reinterpret_cast<uint8*>(PCM16.GetData()), PCM16.Num() * sizeof(int16));

    if (CVarO3DSRemoteAudioDebug->GetInt() != 0)
    {
        static int32 LogEvery = 0;
        if ((LogEvery++ % 50) == 0)
        {
            UE_LOG(LogO3DReceiverAudio, Verbose, TEXT("Queued frames=%d ch=%d sr=%d stream='%s' subject='%s'"),
                NumFrames,
                NumChannels,
                SampleRate,
                *StreamLabel,
                *SubjectName);
        }
    }
}

void UO3DRemoteAudioComponent::OnAudioPcm16(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes)
{
    const FString& StreamLabel = Meta.StreamLabel;
    const FString& SubjectName = Meta.SubjectName;
    if (!MatchesFilter(SubjectName, StreamLabel))
    {
        return;
    }

    const int32 NumSamples = PCM16Bytes.Num() / sizeof(int16);
    if (NumSamples <= 0)
    {
        return;
    }

    const int32 NumChannels = Meta.NumChannels > 0 ? Meta.NumChannels : 1;
    const int32 SampleRate = Meta.SampleRate > 0 ? Meta.SampleRate : 48000;
    EnsureSoundWave(NumChannels, SampleRate);
    if (!SoundWave)
    {
        return;
    }

    static bool bFirstFrame = false;
    if (!bFirstFrame)
    {
        bFirstFrame = true;
        UE_LOG(LogO3DReceiverAudio, Log, TEXT("First PCM16 frame received (%d samples) stream='%s' subject='%s'"), NumSamples, *StreamLabel, *SubjectName);
    }

    if (AudioComp)
    {
        if (!AudioComp->IsPlaying())
        {
            AudioComp->Play();
        }
        AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, AC_VolumeMultiplier * Gain));
    }

    SoundWave->QueueAudio(const_cast<uint8*>(PCM16Bytes.GetData()), PCM16Bytes.Num());
}
