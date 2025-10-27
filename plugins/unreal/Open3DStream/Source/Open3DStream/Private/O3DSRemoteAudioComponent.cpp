#include "O3DSRemoteAudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Components/AudioComponent.h"
#include "AudioDevice.h"
#include "IWebRTCConnector.h"
#include "Open3DStreamSourceSettings.h"

static TSharedPtr<IWebRTCConnector> GetActiveConnector()
{
	static TSharedPtr<IWebRTCConnector> Cached;
	if (!Cached)
	{
		Cached = CreateWebRTCConnector(EO3DSWebRtcBackendReceiver::LibDataChannel, nullptr);
	}
	return Cached;
}

UO3DSRemoteAudioComponent::UO3DSRemoteAudioComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSRemoteAudioComponent::BeginPlay()
{
	Super::BeginPlay();
	if (TSharedPtr<IWebRTCConnector> Conn = GetActiveConnector())
	{
		DelegateHandle = Conn->OnRemoteAudio().AddLambda([this](const FString& StreamLabel, const FString& Subject, const float* PCM, int32 NumFrames, int32 NumChannels, int32 SampleRate)
		{
			const bool bIsMix = StreamLabel.StartsWith(TEXT("o3ds:mix"));
			const bool bMatch = (SubjectName.IsNone() && bIsMix) || (!SubjectName.IsNone() && SubjectName.ToString().Equals(Subject, ESearchCase::IgnoreCase));
			if (!bMatch || NumFrames <=0 || PCM == nullptr) return;
			// Convert to int16 for QueueAudio
			TArray<int16> Pcm16; Pcm16.AddUninitialized(NumFrames * NumChannels);
			for (int32 i=0;i<NumFrames * NumChannels;++i) { float v = FMath::Clamp(PCM[i], -1.0f,1.0f); Pcm16[i] = (int16)FMath::RoundToInt(v *32767.0f); }
			EnsureAudioObjects(NumChannels, SampleRate);
			ProcWave->QueueAudio(reinterpret_cast<const uint8*>(Pcm16.GetData()), Pcm16.Num() * sizeof(int16));
		});
	}
}

void UO3DSRemoteAudioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (TSharedPtr<IWebRTCConnector> Conn = GetActiveConnector())
	{
		if (DelegateHandle.IsValid())
		{
			Conn->OnRemoteAudio().Remove(DelegateHandle);
			DelegateHandle.Reset();
		}
	}
	Super::EndPlay(EndPlayReason);
}

void UO3DSRemoteAudioComponent::EnsureAudioObjects(int32 NumChannels, int32 SampleRate)
{
	if (!ProcWave)
	{
		ProcWave = NewObject<USoundWaveProcedural>(this);
		ProcWave->SetSampleRate(SampleRate);
		ProcWave->NumChannels = NumChannels;
		ProcWave->bLooping = false;
		ProcWave->bProcedural = true;
		ProcWave->Duration = INDEFINITELY_LOOPING_DURATION;
	}
	if (!AudioComponent)
	{
		AudioComponent = NewObject<UAudioComponent>(this);
		AudioComponent->bAutoActivate = false;
		AudioComponent->SetSound(ProcWave);
		AudioComponent->RegisterComponent();
		if (bAutoPlay)
		{
			AudioComponent->Play();
		}
	}
	AudioComponent->SetVolumeMultiplier(Volume);
}
