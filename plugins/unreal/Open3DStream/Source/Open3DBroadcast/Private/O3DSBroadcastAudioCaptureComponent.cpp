// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastAudioCaptureComponent.h"
#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "ISubmixBufferListener.h"
#include "Sound/SoundSubmix.h"
#include "AudioCaptureCore.h"
// Needed for GetWorld() usage in BeginPlay/EndPlay
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
	class FSubmixTap : public ISubmixBufferListener
	{
	public:
		explicit FSubmixTap(UO3DSBroadcastAudioCaptureComponent* InOwner) : Owner(InOwner) {}

		virtual void OnNewSubmixBuffer(const USoundSubmix* /*OwningSubmix*/, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override
		{
			UO3DSBroadcastAudioCaptureComponent* StrongOwner = Owner.Get();
			if (!StrongOwner) return;
			const int32 NumFrames = (NumChannels >0) ? (NumSamples / NumChannels) :0;
			StrongOwner->PushFrames(AudioData, NumFrames, NumChannels, SampleRate, AudioClock);
		}

	private:
		TWeakObjectPtr<UO3DSBroadcastAudioCaptureComponent> Owner;
	};
}

// Opt-in verbose logging for audio capture path (Mix/Mic)
static TAutoConsoleVariable<int32> CVarO3DSAudioCaptureDebug(
	TEXT("o3ds.AudioCapture.Debug"),
	0,
	TEXT("Enable debug logs for O3DS audio capture component (0/1)."),
	ECVF_Default);

UO3DSBroadcastAudioCaptureComponent::UO3DSBroadcastAudioCaptureComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UO3DSBroadcastAudioCaptureComponent::OnRegister()
{
	Super::OnRegister();
	// Keep low-level source in sync with high-level mode as early as possible
	switch (CaptureMode)
	{
		case EO3DSCaptureMode::Mix: Config.Source = EO3DSAudioCaptureSource::GameSubmix; break;
		case EO3DSCaptureMode::Input: Config.Source = EO3DSAudioCaptureSource::Microphone; break;
		default: break;
	}
}

void UO3DSBroadcastAudioCaptureComponent::InitializeComponent()
{
	Super::InitializeComponent();
	// No connector-related setup here anymore - component is now a pure PCM source
}

void UO3DSBroadcastAudioCaptureComponent::BeginPlay()
{
	Super::BeginPlay();

	// Sync high-level mode into low-level source selection
	switch (CaptureMode)
	{
		case EO3DSCaptureMode::Mix: Config.Source = EO3DSAudioCaptureSource::GameSubmix; break;
		case EO3DSCaptureMode::Input: Config.Source = EO3DSAudioCaptureSource::Microphone; break;
		default: break;
	}

	if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
	{
		UE_LOG(LogTemp, Log, TEXT("O3DS AudioCapture: BeginPlay mode=%s sr=%d ch=%d kbps=%d label=%s"),
			(CaptureMode == EO3DSCaptureMode::Mix) ? TEXT("Mix") : TEXT("Input"),
			Config.SampleRate, Config.NumChannels, Config.BitrateKbps, *StreamLabel);
	}

	if (FAudioDevice* AudioDevice = GetWorld() ? GetWorld()->GetAudioDeviceRaw() : nullptr)
	{
		Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(AudioDevice);
		if (Mixer && CaptureMode == EO3DSCaptureMode::Mix)
		{
			if (!SubmixTap.IsValid())
			{
				SubmixTap = MakeShared<FSubmixTap, ESPMode::ThreadSafe>(this);
			}
			USoundSubmix* TargetSubmix = Config.SubmixToTap ? Config.SubmixToTap : &Mixer->GetMainSubmixObject();
			if (TargetSubmix)
			{
				Mixer->RegisterSubmixBufferListener(SubmixTap.ToSharedRef(), *TargetSubmix);
				if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
				{
					UE_LOG(LogTemp, Log, TEXT("O3DS AudioCapture: Submix tap registered on %s"), *GetNameSafe(TargetSubmix));
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("O3DS AudioCapture: No target submix available for tap registration"));
			}
		}
	}

	// Optional input capture
	if (CaptureMode == EO3DSCaptureMode::Input)
	{
		MicCapture = new Audio::FAudioCapture();
		Audio::FAudioCaptureDeviceParams Params; // default device unless overridden
		Params.DeviceIndex = Config.DeviceIndex; // -1 uses default
		Audio::FOnAudioCaptureFunction OnCapture = [this](const void* Buffer, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTimeSec, bool /*bOverflow*/)
		{
			const float* PCM = reinterpret_cast<const float*>(Buffer);
			this->PushFrames(PCM, NumFrames, NumChannels, SampleRate, StreamTimeSec);
		};
		if (MicCapture && MicCapture->OpenAudioCaptureStream(Params, OnCapture, /*NumFramesDesired*/0))
		{
			MicCapture->StartStream();
			if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
			{
				UE_LOG(LogTemp, Log, TEXT("O3DS AudioCapture: Mic stream started (DeviceIndex=%d)"), Config.DeviceIndex);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("O3DS AudioCapture: Failed to open mic stream (DeviceIndex=%d)"), Config.DeviceIndex);
		}
	}
}

void UO3DSBroadcastAudioCaptureComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (FAudioDevice* AudioDevice = GetWorld() ? GetWorld()->GetAudioDeviceRaw() : nullptr)
	{
		Audio::FMixerDevice* Mixer = static_cast<Audio::FMixerDevice*>(AudioDevice);
		if (Mixer && SubmixTap.IsValid())
		{
			USoundSubmix* TargetSubmix = Config.SubmixToTap ? Config.SubmixToTap : &Mixer->GetMainSubmixObject();
			if (TargetSubmix)
			{
				Mixer->UnregisterSubmixBufferListener(SubmixTap.ToSharedRef(), *TargetSubmix);
			}
			SubmixTap.Reset();
		}
	}

	if (MicCapture)
	{
		MicCapture->StopStream();
		MicCapture->CloseStream();
		delete MicCapture;
		MicCapture = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

void UO3DSBroadcastAudioCaptureComponent::SetStreamLabel(const FString& InLabel)
{
	StreamLabel = InLabel;
	if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
	{
		UE_LOG(LogTemp, Log, TEXT("O3DS AudioCapture: StreamLabel set to '%s'"), *StreamLabel);
	}
}

void UO3DSBroadcastAudioCaptureComponent::SetAudioSink(TFunction<bool(const FString&, const float*, int32, int32, int32, double)> InSink)
{
	AudioSink = MoveTemp(InSink);
	if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
	{
		UE_LOG(LogTemp, Log, TEXT("O3DS AudioCapture: AudioSink configured"));
	}
}

void UO3DSBroadcastAudioCaptureComponent::PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	// Forward frames to sink if configured, otherwise silently drop
	if (!AudioSink)
	{
		return;
	}
	
	const bool bSuccess = AudioSink(StreamLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
	
	if (!bSuccess)
	{
		// Throttle warnings to avoid spam
		static double sLastWarn = 0.0;
		const double Now = FPlatformTime::Seconds();
		if (Now - sLastWarn > 1.0)
		{
			UE_LOG(LogTemp, Warning, TEXT("[AudioCapture] Sink rejected frame (StreamLabel=%s, NumFrames=%d)"),
				*StreamLabel, NumFrames);
			sLastWarn = Now;
		}
	}
	
	if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
	{
		static int32 LogEvery =0; if ((LogEvery++ %50) ==0)
		{
			UE_LOG(LogTemp, Verbose, TEXT("O3DS AudioCapture: PushFrames label=%s frames=%d ch=%d sr=%d ok=%d"),
				*StreamLabel, NumFrames, NumChannels, SampleRate, bSuccess ?1 :0);
		}
	}
}

TArray<FName> UO3DSBroadcastAudioCaptureComponent::GetAvailableInputDeviceOptions() const
{
	TArray<FName> Options;
	Audio::FAudioCapture Temp;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	if (Temp.GetCaptureDevicesAvailable(Devices) >0)
	{
		for (int32 i =0; i < Devices.Num(); ++i)
		{
			Options.Add(FName(*Devices[i].DeviceName));
		}
	}
	return Options;
}

int32 UO3DSBroadcastAudioCaptureComponent::ResolveDeviceIndexFromName(const FName& Name) const
{
	if (Name.IsNone()) return -1;
	Audio::FAudioCapture Temp;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	if (Temp.GetCaptureDevicesAvailable(Devices) >0)
	{
		for (int32 i =0; i < Devices.Num(); ++i)
		{
			if (Devices[i].DeviceName.Equals(Name.ToString(), ESearchCase::IgnoreCase))
			{
				return i;
			}
		}
	}
	return -1;
}

#if WITH_EDITOR
void UO3DSBroadcastAudioCaptureComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName Prop = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
	
	// Sync capture mode to config source
	if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSBroadcastAudioCaptureComponent, CaptureMode))
	{
		Config.Source = (CaptureMode == EO3DSCaptureMode::Mix) ? EO3DSAudioCaptureSource::GameSubmix : EO3DSAudioCaptureSource::Microphone;
	}
	else if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSBroadcastAudioCaptureComponent, InputDeviceName))
	{
		Config.DeviceIndex = ResolveDeviceIndexFromName(InputDeviceName);
	}
	
	// Component is now a pure PCM source - no automatic reconfiguration needed
	// BroadcastComponent will handle setup when StartCapture() is called
}
#endif
