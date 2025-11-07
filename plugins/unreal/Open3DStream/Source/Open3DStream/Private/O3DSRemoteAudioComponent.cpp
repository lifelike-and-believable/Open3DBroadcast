// Copyright (c) Open3DStream Contributors

#include "O3DSRemoteAudioComponent.h"

#include "Sound/SoundWaveProcedural.h"
#include "Components/AudioComponent.h"
#include "O3DSAudioBus.h"
#include "O3DSUnifiedMessage.h"
#include "O3DSStreamLogs.h"
// Needed for AActor definition used by GetOwner() and attachment calls
#include "GameFramework/Actor.h"
#include "Sound/SoundAttenuation.h"
// For scene component attachment
#include "Components/SceneComponent.h"
// Submix/Effects/Concurrency/Modulation support
#include "Sound/SoundSubmix.h"
#include "Sound/SoundSubmixSend.h"
#include "Sound/SoundConcurrency.h"

// Opt-in verbose logging for remote audio receive/playback
static TAutoConsoleVariable<int32> CVarO3DSRemoteAudioDebug(
	TEXT("o3ds.RemoteAudio.Debug"),
	0,
	TEXT("Enable debug logs for O3DS remote audio component (0/1)."),
	ECVF_Default);

UO3DSRemoteAudioComponent::UO3DSRemoteAudioComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSRemoteAudioComponent::OnRegister()
{
	Super::OnRegister();

	// Attach this SceneComponent to the specified parent (or owner's root)
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

void UO3DSRemoteAudioComponent::BeginPlay()
{
	Super::BeginPlay();

	// Always create and configure a dedicated AudioComponent
	AudioComp = NewObject<UAudioComponent>(GetOwner());
	if (AudioComp)
	{
		AudioComp->RegisterComponent();
		// Attach internal audio component to this SceneComponent for consistent transform inheritance
		AudioComp->AttachToComponent(this, FAttachmentTransformRules::KeepRelativeTransform);
		// Configure from mirrored properties (leave out Sound Source)
		AudioComp->bAutoActivate = bAC_AutoActivate;
		AudioComp->bAllowSpatialization = bAC_AllowSpatialization;
		AudioComp->bIsUISound = bAC_IsUISound;
		AudioComp->bOverrideAttenuation = bAC_OverrideAttenuation;
		AudioComp->AttenuationSettings = AC_AttenuationSettings;
		AudioComp->SetPitchMultiplier(AC_PitchMultiplier);
		// Effective volume = user multiplier * Gain
		AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, AC_VolumeMultiplier * Gain));
		bOwnsAudioComponent = true;

		// Advanced settings (submix/effects/concurrency) applied on SoundWave when created
	}

	// Subscribe to global audio bus published by the network receiver
	BusDelegateHandle = FO3DSAudioBus::OnPcm16().AddUObject(this, &UO3DSRemoteAudioComponent::OnAudioPcm16);
	// Always emit a single confirmation log so users can verify subscription without CVars
	{
		static bool bOnce = false;
		if (!bOnce)
		{
			bOnce = true;
			UE_LOG(LogO3DSReceiverAudio, Log, TEXT("Subscribed to FO3DSAudioBus (Gain=%.2f)"), Gain);
			if (!AudioComp)
			{
				UE_LOG(LogO3DSReceiverAudio, Warning, TEXT("No UAudioComponent present/created on owner '%s'"), *GetOwner()->GetName());
			}
		}
	}
	if (CVarO3DSRemoteAudioDebug->GetInt() !=0)
	{
		UE_LOG(LogO3DSReceiverAudio, Log, TEXT("Debug enabled"));
	}
}

void UO3DSRemoteAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (BusDelegateHandle.IsValid())
	{
		FO3DSAudioBus::OnPcm16().Remove(BusDelegateHandle);
		BusDelegateHandle.Reset();
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
	if (CVarO3DSRemoteAudioDebug->GetInt() !=0)
	{
		UE_LOG(LogO3DSReceiverAudio, Verbose, TEXT("Filter pass subject='%s' stream='%s'"), *InSubject, *InStream);
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
			SoundWave->SetSampleRate(SampleRate);
			SoundWave->Duration = INDEFINITELY_LOOPING_DURATION;
			SoundWave->SoundGroup = SOUNDGROUP_Voice;
			// Explicitly mark as procedural/streaming source
			SoundWave->bProcedural = true;
			// Apply mirrored settings that are defined on USoundBase/USoundWave
			SoundWave->SourceEffectChain = AC_SourceEffectChain;
			SoundWave->SoundSubmixSends = AC_SubmixSends;
			// Concurrency
			SoundWave->ConcurrencyOverrides = AC_ConcurrencyOverrides;
			SoundWave->ConcurrencySet.Reset();
			for (TObjectPtr<USoundConcurrency> Concurrency : AC_ConcurrencySet)
			{
				if (Concurrency)
				{
					SoundWave->ConcurrencySet.Add(Concurrency);
				}
			}
			// Modulation destinations (omitted here if not available on this engine version)
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
			// Always apply effective volume = user multiplier * Gain
			AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, AC_VolumeMultiplier * Gain));
			if (CVarO3DSRemoteAudioDebug->GetInt() !=0)
			{
				UE_LOG(LogO3DSReceiverAudio, Log, TEXT("SoundWave prepared ch=%d sr=%d; AudioComp playing=%d"),
					NumChannels, SampleRate, AudioComp->IsPlaying()?1:0);
			}
		}
	}
}

void UO3DSRemoteAudioComponent::OnAudioFrame(const FString& StreamLabel, const FString& SubjectName, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate)
{
	if (!MatchesFilter(SubjectName, StreamLabel))
	{
		if (CVarO3DSRemoteAudioDebug->GetInt() !=0)
		{
			static int32 DropEvery =0; if ((DropEvery++ %100) ==0)
			{
				UE_LOG(LogO3DSReceiverAudio, Verbose, TEXT("Dropped frame by filter subject='%s' stream='%s'"), *SubjectName, *StreamLabel);
			}
		}
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

	if (CVarO3DSRemoteAudioDebug->GetInt() !=0)
	{
		static int32 LogEvery =0; if ((LogEvery++ %50) ==0)
		{
			UE_LOG(LogO3DSReceiverAudio, Verbose, TEXT("Queued frames=%d ch=%d sr=%d stream='%s' subject='%s'"),
				NumFrames, NumChannels, SampleRate, *StreamLabel, *SubjectName);
		}
	}
}

void UO3DSRemoteAudioComponent::OnAudioPcm16(const O3DS::FAudioFrameMeta& Meta, const TArray<uint8>& PCM16Bytes)
{
	const FString& StreamLabel = Meta.StreamLabel;
	const FString& SubjectName = Meta.SubjectName;
	if (!MatchesFilter(SubjectName, StreamLabel))
	{
		return;
	}
	const int32 NumSamples = PCM16Bytes.Num() / sizeof(int16);
	if (NumSamples <=0)
	{
		return;
	}
	// Queue PCM16 directly (no conversion)
	EnsureSoundWave(Meta.NumChannels >0 ? Meta.NumChannels :1, Meta.SampleRate >0 ? Meta.SampleRate :48000);
	if (!SoundWave)
	{
		return;
	}
	// One-time confirmation log on first received frame
	{
		static bool bFirstFrame = false;
		if (!bFirstFrame)
		{
			bFirstFrame = true;
			UE_LOG(LogO3DSReceiverAudio, Log, TEXT("First PCM16 frame received (%d samples) stream='%s' subject='%s'"), NumSamples, *StreamLabel, *SubjectName);
		}
	}
	// Keep audio component playing; ensure effective volume applied even when user changes settings at runtime
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
