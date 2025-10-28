// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastAudioCaptureComponent.h"
#include "AudioDevice.h"
#include "AudioMixerDevice.h"
#include "ISubmixBufferListener.h"
#include "IWebRTCConnector.h"
#include "Sound/SoundSubmix.h"
#include "AudioCaptureCore.h"
// Needed for GetWorld() usage in BeginPlay/EndPlay
#include "Engine/World.h"

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

	if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
	{
		UE_LOG(LogTemp, Log, TEXT("O3DS AudioCapture: BeginPlay mode=%s sr=%d ch=%d kbps=%d"),
			(CaptureMode == EO3DSCaptureMode::Mix) ? TEXT("Mix") : TEXT("Input"),
			Config.SampleRate, Config.NumChannels, Config.BitrateKbps);
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

void UO3DSBroadcastAudioCaptureComponent::SetConnector(TSharedPtr<IWebRTCConnector> InConnector)
{
	Connector = InConnector;
	// Reset warning throttle so if connector goes null later, we warn once again
	bWarnedNoConnector = false;
	// Attempt to configure send immediately if possible
	EnsureConnector();
}

void UO3DSBroadcastAudioCaptureComponent::EnsureConnector()
{
	if (!Connector)
	{
		// Do not auto-fetch a shared connector anymore. Leave null to surface networking issues.
		if (CVarO3DSAudioCaptureDebug->GetInt() !=0 && !bWarnedNoConnector)
		{
			UE_LOG(LogTemp, Warning, TEXT("O3DS AudioCapture: No connector set on component"));
			bWarnedNoConnector = true;
		}
		return;
	}

	// Connector is valid again; allow future warnings if it becomes null later
	bWarnedNoConnector = false;

	// If connector was set externally, (re)configure send params
	IWebRTCConnector::FAudioSendConfig A;
	A.bEnable = true;
	A.SampleRate = Config.SampleRate;
	A.NumChannels = Config.NumChannels;
	A.BitrateKbps = Config.BitrateKbps;

	// Choose StreamLabel based on mode and subject/device
	if (CaptureMode == EO3DSCaptureMode::Input)
	{
		if (!SubjectName.IsNone())
		{
			StreamLabel = FString::Printf(TEXT("o3ds:subject/%s"), *SubjectName.ToString());
		}
		else if (!InputDeviceName.IsNone())
		{
			StreamLabel = FString::Printf(TEXT("o3ds:mic/%s"), *InputDeviceName.ToString());
		}
		else
		{
			StreamLabel = TEXT("o3ds:mic");
		}
		A.SourceType = TEXT("mic");
	}
	else // Mix
	{
		StreamLabel = SubjectName.IsNone() ? FString(TEXT("o3ds:mix")) : FString::Printf(TEXT("o3ds:subject/%s"), *SubjectName.ToString());
		A.SourceType = TEXT("mix");
	}

	A.StreamLabel = StreamLabel;
	A.SubjectName = SubjectName.ToString();

	const bool bEnabled = Connector->EnableAudioSend(A);
	if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
	{
		UE_LOG(LogTemp, Log, TEXT("O3DS AudioCapture: Connector ready=%d EnableAudioSend label=%s subject=%s sr=%d ch=%d br=%d -> %s"),
			Connector.IsValid()?1:0,
			*A.StreamLabel, *A.SubjectName, A.SampleRate, A.NumChannels, A.BitrateKbps,
			bEnabled ? TEXT("OK") : TEXT("FAILED"));
	}
}

void UO3DSBroadcastAudioCaptureComponent::PushFrames(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	EnsureConnector();
	if (!Connector) return;
	const bool bPushed = Connector->PushPcm(StreamLabel, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec);
	if (CVarO3DSAudioCaptureDebug->GetInt() !=0)
	{
		static int32 LogEvery =0; if ((LogEvery++ %50) ==0)
		{
			UE_LOG(LogTemp, Verbose, TEXT("O3DS AudioCapture: PushFrames label=%s frames=%d ch=%d sr=%d ok=%d"),
				*StreamLabel, NumFrames, NumChannels, SampleRate, bPushed ?1 :0);
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
