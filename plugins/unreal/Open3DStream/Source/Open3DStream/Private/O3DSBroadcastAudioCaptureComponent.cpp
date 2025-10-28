#include "O3DSBroadcastAudioCaptureComponent.h"
#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "ISubmixBufferListener.h"
#include "IWebRTCConnector.h"
#include "O3DSWebRTCService.h"
#include "Sound/SoundSubmix.h"
#include "AudioCaptureCore.h"

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

static TSharedPtr<IWebRTCConnector> GetSharedConnector()
{
	return UO3DSWebRTCService::Get()->GetConnector();
}

UO3DSBroadcastAudioCaptureComponent::UO3DSBroadcastAudioCaptureComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
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

	EnsureConnector();

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

void UO3DSBroadcastAudioCaptureComponent::EnsureConnector()
{
	if (!Connector)
	{
		Connector = GetSharedConnector();
		if (Connector)
		{
			IWebRTCConnector::FAudioSendConfig A;
			A.bEnable = true;
			A.SampleRate = Config.SampleRate;
			A.NumChannels = Config.NumChannels;
			A.BitrateKbps = Config.BitrateKbps;
			StreamLabel = SubjectName.IsNone() ? FString(TEXT("o3ds:mix")) : FString::Printf(TEXT("o3ds:subject/%s"), *SubjectName.ToString());
			A.StreamLabel = StreamLabel;
			A.SubjectName = SubjectName.ToString();
			A.SourceType = (CaptureMode == EO3DSCaptureMode::Mix) ? TEXT("mix") : TEXT("mic");
			Connector->EnableAudioSend(A);
		}
	}
}

void UO3DSBroadcastAudioCaptureComponent::PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	EnsureConnector();
	if (!Connector) return;
	Connector->PushPcm(StreamLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
}

TArray<FName> UO3DSBroadcastAudioCaptureComponent::GetAvailableInputDeviceOptions() const
{
	TArray<FName> Options;
	Audio::FAudioCapture Temp;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	if (Temp.GetCaptureDevicesAvailable(Devices) >0)
	{
		for (int32 i=0;i<Devices.Num();++i)
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
		for (int32 i=0;i<Devices.Num();++i)
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
	if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSBroadcastAudioCaptureComponent, CaptureMode))
	{
		Config.Source = (CaptureMode == EO3DSCaptureMode::Mix) ? EO3DSAudioCaptureSource::GameSubmix : EO3DSAudioCaptureSource::Microphone;
	}
	else if (Prop == GET_MEMBER_NAME_CHECKED(UO3DSBroadcastAudioCaptureComponent, InputDeviceName))
	{
		Config.DeviceIndex = ResolveDeviceIndexFromName(InputDeviceName);
	}
}
#endif
