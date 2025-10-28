// Copyright (c) Open3DStream Contributors

#include "O3DSRemoteAudioComponent.h"

#include "IWebRTCConnector.h"
#include "O3DSWebRTCService.h"
#include "Sound/SoundWaveProcedural.h"
#include "Components/AudioComponent.h"

UO3DSRemoteAudioComponent::UO3DSRemoteAudioComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSRemoteAudioComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoCreateAudioComponent)
	{
		AudioComp = GetOwner() ? GetOwner()->FindComponentByClass<UAudioComponent>() : nullptr;
		if (!AudioComp)
		{
			AudioComp = NewObject<UAudioComponent>(GetOwner());
			if (AudioComp)
			{
				AudioComp->RegisterComponent();
				if (GetOwner() && GetOwner()->GetRootComponent())
				{
					AudioComp->AttachToComponent(GetOwner()->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
				}
			}
		}
	}

	if (TSharedPtr<IWebRTCConnector> Conn = UO3DSWebRTCService::Get()->GetConnector())
	{
		AudioDelegateHandle = Conn->OnRemoteAudio().AddUObject(this, &UO3DSRemoteAudioComponent::OnAudioFrame);
	}
}

void UO3DSRemoteAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (TSharedPtr<IWebRTCConnector> Conn = UO3DSWebRTCService::Get()->GetConnector())
	{
		if (AudioDelegateHandle.IsValid())
		{
			Conn->OnRemoteAudio().Remove(AudioDelegateHandle);
			AudioDelegateHandle.Reset();
		}
	}
	Super::EndPlay(EndPlayReason);
}

bool UO3DSRemoteAudioComponent::MatchesFilter(const FString& InSubject, const FString& InStream) const
{
	const bool bIsMix = InStream.StartsWith(TEXT("o3ds:mix"));
	bool bSubjectMatch = false;
	if (ReceiveMode == EO3DSRemoteAudioMode::Mix)
	{
		bSubjectMatch = bIsMix;
	}
	else
	{
		const FString Desired = LiveLinkSubjectName.Name.ToString();
		bSubjectMatch = !Desired.IsEmpty() && InSubject.Equals(Desired, ESearchCase::IgnoreCase);
	}
	if (!bSubjectMatch) return false;
	if (!StreamLabelFilter.IsEmpty() && !InStream.Contains(StreamLabelFilter)) return false;
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
			SoundWave->SetSampleRate(SampleRate);
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

void UO3DSRemoteAudioComponent::OnAudioFrame(const FString& StreamLabel, const FString& SubjectName, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate)
{
	if (!MatchesFilter(SubjectName, StreamLabel))
	{
		return;
	}
	EnsureSoundWave(NumChannels, SampleRate);
	if (!SoundWave || NumFrames <=0 || NumChannels <=0)
	{
		return;
	}

	// Convert float [-1,1] to int16 and queue
	const int32 NumSamples = NumFrames * NumChannels;
	TArray<int16> PCM16;
	PCM16.AddUninitialized(NumSamples);
	for (int32 i =0; i < NumSamples; ++i)
	{
		float v = Interleaved[i] * Gain;
		v = FMath::Clamp(v, -1.0f,1.0f);
		PCM16[i] = (int16)FMath::RoundToInt(v *32767.0f);
	}
	SoundWave->QueueAudio(reinterpret_cast<uint8*>(PCM16.GetData()), PCM16.Num() * sizeof(int16));
}
