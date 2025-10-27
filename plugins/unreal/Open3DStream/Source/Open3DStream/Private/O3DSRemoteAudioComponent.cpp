// Copyright (c) Open3DStream Contributors

#include "O3DSRemoteAudioComponent.h"

#include "Sound/SoundWaveProcedural.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"

UO3DSRemoteAudioComponent::UO3DSRemoteAudioComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSRemoteAudioComponent::BeginPlay()
{
    Super::BeginPlay();

    // Subscribe to audio bus
    BusHandle = FO3DSAudioBus::OnPcm16().AddUObject(this, &UO3DSRemoteAudioComponent::OnAudioFrame);

    if (bAutoCreateAudioComponent)
    {
        AudioComp = GetOwner() ? GetOwner()->FindComponentByClass<UAudioComponent>() : nullptr;
        if (!AudioComp)
        {
            AudioComp = NewObject<UAudioComponent>(GetOwner());
            if (AudioComp)
            {
                AudioComp->RegisterComponent();
                AudioComp->AttachToComponent(GetOwner()->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
            }
        }
    }
}

void UO3DSRemoteAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (BusHandle.IsValid())
    {
        FO3DSAudioBus::OnPcm16().Remove(BusHandle);
        BusHandle.Reset();
    }
    Super::EndPlay(EndPlayReason);
}

bool UO3DSRemoteAudioComponent::MatchesFilter(const O3DS::FAudioFrameMeta& Meta) const
{
    // Stream label filter (simple contains match if non-empty)
    if (!StreamLabelFilter.IsEmpty() && !Meta.StreamLabel.Contains(StreamLabelFilter))
    {
        return false;
    }
    if (!SubjectNameFilter.IsEmpty() && !Meta.SubjectName.IsEmpty())
    {
        if (!Meta.SubjectName.Equals(SubjectNameFilter, ESearchCase::IgnoreCase))
        {
            return false;
        }
    }
    return true;
}

void UO3DSRemoteAudioComponent::EnsureSoundWave(int32 NumChannels, int32 SampleRate)
{
    const bool bNeedNew = (SoundWave == nullptr) || (CurrentChannels != NumChannels) || (CurrentSampleRate != SampleRate);
    if (bNeedNew)
    {
        SoundWave = NewObject<USoundWaveProcedural>(this);
        if (SoundWave)
        {
            SoundWave->bLooping = false;
            SoundWave->NumChannels = NumChannels;
            SoundWave->SampleRate = SampleRate;
            SoundWave->Duration = INDEFINITELY_LOOPING_DURATION;
            SoundWave->SoundGroup = SOUNDGROUP_Voice;
        }
        CurrentChannels = NumChannels;
        CurrentSampleRate = SampleRate;

        if (AudioComp)
        {
            AudioComp->SetSound(SoundWave);
            if (!AudioComp->IsPlaying())
            {
                AudioComp->Play();
            }
        }
    }
}

void UO3DSRemoteAudioComponent::OnAudioFrame(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes)
{
    if (!MatchesFilter(Meta))
    {
        return;
    }

    if (PCM16Bytes.Num() == 0)
    {
        return;
    }

    EnsureSoundWave(Meta.NumChannels, Meta.SampleRate);

    if (!SoundWave)
    {
        return;
    }

    // Optional gain: multiply samples in-place (16-bit signed)
    if (!FMath::IsNearlyEqual(Gain, 1.0f))
    {
        TArray<uint8> Scaled = PCM16Bytes; // copy
        int16* Samples = reinterpret_cast<int16*>(Scaled.GetData());
        const int32 Count = Scaled.Num() / sizeof(int16);
        for (int32 i = 0; i < Count; ++i)
        {
            const float F = FMath::Clamp((float)Samples[i] * Gain, -32768.0f, 32767.0f);
            Samples[i] = (int16)F;
        }
        SoundWave->QueueAudio(Scaled.GetData(), Scaled.Num());
    }
    else
    {
        SoundWave->QueueAudio(PCM16Bytes.GetData(), PCM16Bytes.Num());
    }
}
