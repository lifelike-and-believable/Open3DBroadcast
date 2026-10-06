// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#include "O3DRemoteAudioComponent.h"

#include "O3DRuntimeSubsystem.h"

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
    AActor* Owner = GetOwner();
    if (!Owner)
    {
        return;
    }

    // RCV-22: an unset reference resolves to the root, so only a reference the user set may move a
    // component they placed; otherwise only a component with no parent attaches, and never to itself.
    USceneComponent* ParentToAttach = nullptr;
    if (!(AC_AttachParent == FComponentReference()))
    {
        ParentToAttach = Cast<USceneComponent>(AC_AttachParent.GetComponent(Owner));
    }
    else if (GetAttachParent() == nullptr)
    {
        ParentToAttach = Owner->GetRootComponent();
    }

    if (ParentToAttach && ParentToAttach != this && ParentToAttach != GetAttachParent())
    {
        AttachToComponent(ParentToAttach, FAttachmentTransformRules::KeepRelativeTransform, AC_AttachSocketName);
    }
}

void UO3DRemoteAudioComponent::BeginPlay()
{
    Super::BeginPlay();

    bPlaybackWanted = bAC_AutoActivate;
    AudioComp = NewObject<UAudioComponent>(GetOwner());
    if (AudioComp)
    {
        // Set before RegisterComponent, which activates (plays) an auto-activating component. This
        // component starts playback itself once a sound exists (RCV-23).
        AudioComp->bAutoActivate = false;
        AudioComp->bAllowSpatialization = bAC_AllowSpatialization;
        AudioComp->bIsUISound = bAC_IsUISound;
        AudioComp->bOverrideAttenuation = bAC_OverrideAttenuation;
        AudioComp->AttenuationSettings = AC_AttenuationSettings;
        //AudioComp->AttenuationOverrides = AC_AttenuationOverrides;
        AudioComp->SetPitchMultiplier(AC_PitchMultiplier);
        AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, AC_VolumeMultiplier * Gain));
        AudioComp->SetupAttachment(this);
        AudioComp->RegisterComponent();
    }

    BindBus();

    // Per component (RCV-25): a function-static flag logged only for the first component in the process.
    bLoggedFirstFrame = false;
    UE_LOG(LogO3DReceiverAudio, Log, TEXT("Subscribed to the audio bus of context '%s' (Gain=%.2f, owner '%s')"), *ContextName.ToString(), Gain, *GetNameSafe(GetOwner()));
    if (!AudioComp)
    {
        UE_LOG(LogO3DReceiverAudio, Warning, TEXT("No UAudioComponent present/created on owner '%s'"), *GetNameSafe(GetOwner()));
    }

    if (CVarO3DSRemoteAudioDebug->GetInt() != 0)
    {
        UE_LOG(LogO3DReceiverAudio, Log, TEXT("Remote audio debug enabled"));
    }
}

void UO3DRemoteAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    UnbindBus();

    if (AudioComp)
    {
        AudioComp->Stop();
        AudioComp->DestroyComponent();
        AudioComp = nullptr;
    }
    SoundWave = nullptr;
    CurrentChannels = 0;
    CurrentSampleRate = 0;

    Super::EndPlay(EndPlayReason);
}

void UO3DRemoteAudioComponent::Play()
{
    bPlaybackWanted = true;
    if (AudioComp && SoundWave && !AudioComp->IsPlaying())
    {
        AudioComp->Play();
    }
}

void UO3DRemoteAudioComponent::Stop()
{
    bPlaybackWanted = false;
    if (AudioComp)
    {
        AudioComp->Stop();
    }
}

void UO3DRemoteAudioComponent::BindBus()
{
    if (!BusDelegateHandle.IsValid())
    {
        BoundContext = UO3DRuntimeSubsystem::Resolve(ContextName);
        BusDelegateHandle = BoundContext->GetAudioBus().OnPcm16().AddUObject(this, &UO3DRemoteAudioComponent::OnAudioPcm16);
    }
}

void UO3DRemoteAudioComponent::UnbindBus()
{
    if (BusDelegateHandle.IsValid() && BoundContext.IsValid())
    {
        BoundContext->GetAudioBus().OnPcm16().Remove(BusDelegateHandle);
    }
    BusDelegateHandle.Reset();
    BoundContext.Reset();
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
        const FName DesiredName = LiveLinkSubjectName.Name;
        if (DesiredName.IsNone())
        {
            bSubjectMatch = false;
        }
        else
        {
            const FString Desired = DesiredName.ToString();
            bSubjectMatch = !Desired.IsEmpty() && InSubject.Equals(Desired, ESearchCase::IgnoreCase);
        }
    }

    if (!bSubjectMatch)
    {
        return false;
    }

    // Note: Audio stream label is now automatically derived from SubjectName, no additional filtering needed
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
            if (bPlaybackWanted && !AudioComp->IsPlaying())
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

void UO3DRemoteAudioComponent::OnAudioPcm16(const O3DS::FAudioFrameMeta& Meta, TConstArrayView<uint8> PCM16Bytes)
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

    if (!bLoggedFirstFrame)
    {
        bLoggedFirstFrame = true;
        UE_LOG(LogO3DReceiverAudio, Log, TEXT("First PCM16 frame received (%d samples) stream='%s' subject='%s'"), NumSamples, *StreamLabel, *SubjectName);
    }

    if (AudioComp)
    {
        if (bPlaybackWanted && !AudioComp->IsPlaying())
        {
            AudioComp->Play();
        }
        AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, AC_VolumeMultiplier * Gain));
    }

    SoundWave->QueueAudio(const_cast<uint8*>(PCM16Bytes.GetData()), PCM16Bytes.Num());
}
