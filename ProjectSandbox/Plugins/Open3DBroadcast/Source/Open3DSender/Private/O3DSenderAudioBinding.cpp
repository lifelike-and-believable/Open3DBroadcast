// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DSenderAudioBinding.h"

#include "GameFramework/Actor.h"
#include "O3DAudioFrameCodec.h"
#include "O3DAudioInputDevices.h"
#include "O3DSenderLogs.h"
#include "Sound/SoundSubmix.h"
#include "Transport/O3DSenderInterface.h"

FO3DSenderAudioCaptureConfig FO3DSenderAudioBinding::BuildCaptureConfig(const FO3DSenderAudioSettings& Settings)
{
	FO3DSenderAudioCaptureConfig ConfigCopy = Settings.CaptureConfig;
	ConfigCopy.Source = (Settings.Mode == EO3DSenderCaptureMode::Mix)
		? EO3DSenderAudioSource::GameSubmix
		: EO3DSenderAudioSource::Microphone;

	if (Settings.Mode == EO3DSenderCaptureMode::Input)
	{
		ConfigCopy.DeviceIndex = ResolveDeviceIndex(Settings.InputDevice);
	}

	return ConfigCopy;
}

FO3DTransportAudioConfig FO3DSenderAudioBinding::BuildTransportConfig(const FO3DSenderAudioSettings& Settings, const FO3DSenderAudioCaptureConfig& CaptureConfig)
{
	FO3DTransportAudioConfig AudioConfig;
	AudioConfig.bEnableAudio = Settings.bEnableAudio;
	if (!AudioConfig.bEnableAudio)
	{
		return AudioConfig;
	}

	AudioConfig.SampleRate = CaptureConfig.SampleRate;
	AudioConfig.NumChannels = CaptureConfig.NumChannels;
	AudioConfig.BitrateKbps = CaptureConfig.BitrateKbps;
	AudioConfig.Mode = (Settings.Mode == EO3DSenderCaptureMode::Mix) ? TEXT("mix") : TEXT("input");
	// Audio stream label is automatically derived from SubjectName for logical association on receiver side
	if (Settings.Mode == EO3DSenderCaptureMode::Input)
	{
		AudioConfig.InputDevice = Settings.InputDevice.IsNone() ? FString() : Settings.InputDevice.ToString();
	}
	else
	{
		AudioConfig.InputDevice.Reset();
	}

	const FString CodecString = O3DAudio::SanitizeCodecString(Settings.Codec.IsNone() ? FString() : Settings.Codec.ToString());
	AudioConfig.AdvancedParams.Empty();
	AudioConfig.AdvancedParams.Add(TEXT("game_gain"), FString::SanitizeFloat(CaptureConfig.GameGain));
	AudioConfig.AdvancedParams.Add(TEXT("mic_gain"), FString::SanitizeFloat(CaptureConfig.MicGain));
	if (CaptureConfig.DeviceIndex >= 0)
	{
		AudioConfig.AdvancedParams.Add(TEXT("device_index"), FString::FromInt(CaptureConfig.DeviceIndex));
	}
	if (CaptureConfig.SubmixToTap)
	{
		AudioConfig.AdvancedParams.Add(TEXT("submix"), CaptureConfig.SubmixToTap->GetPathName());
	}
	if (!CodecString.IsEmpty())
	{
		AudioConfig.Codec = CodecString;
		AudioConfig.AdvancedParams.Add(TEXT("codec"), CodecString);
	}
	else
	{
		AudioConfig.Codec.Reset();
	}

	return AudioConfig;
}

void FO3DSenderAudioBinding::SyncSource(EO3DSenderCaptureMode Mode, FName InputDevice, FO3DSenderAudioCaptureConfig& Config)
{
	Config.Source = (Mode == EO3DSenderCaptureMode::Mix)
		? EO3DSenderAudioSource::GameSubmix
		: EO3DSenderAudioSource::Microphone;
	if (Mode == EO3DSenderCaptureMode::Input)
	{
		Config.DeviceIndex = ResolveDeviceIndex(InputDevice);
	}
}

int32 FO3DSenderAudioBinding::ResolveDeviceIndex(FName DeviceName)
{
	return FO3DAudioInputDevices::Get().FindIndex(DeviceName);
}

UO3DSenderAudioCaptureComponent* FO3DSenderAudioBinding::FindOrCreateCaptureComponent(AActor* Owner, UO3DSenderAudioCaptureComponent* Current)
{
	if (Current)
	{
		if (IsValid(Current) && !Current->IsBeingDestroyed())
		{
			return Current;
		}
		Current = nullptr;
	}

	if (!Owner)
	{
		return nullptr;
	}

	Current = Owner->FindComponentByClass<UO3DSenderAudioCaptureComponent>();
	if (Current && Current->GetOwner() != Owner)
	{
		Current = nullptr;
	}

	if (!Current)
	{
		Current = NewObject<UO3DSenderAudioCaptureComponent>(Owner, TEXT("O3DSenderAudioCapture"));
		if (Current)
		{
			Current->SetFlags(RF_Transactional);
			Current->OnComponentCreated();
			Current->RegisterComponent();
			Owner->AddInstanceComponent(Current);
		}
	}
	return Current;
}

void FO3DSenderAudioBinding::Configure(UO3DSenderAudioCaptureComponent& Capture, const FO3DSenderAudioSettings& Settings, const FO3DSenderAudioCaptureConfig& CaptureConfig)
{
	Capture.InputDeviceName = Settings.InputDevice;
	Capture.Config = CaptureConfig;
	// Audio stream label is automatically derived from SubjectName (no longer configurable per component)
	Capture.StartCaptureWithMode(Settings.Mode);
}

void FO3DSenderAudioBinding::AttachSink(UO3DSenderAudioCaptureComponent& Capture, const TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe>& Sink,
	const FString& Label, FName TransportName, double NowSeconds)
{
	Capture.SetAudioSink(Sink, Label);

	if (!Sink.IsValid())
	{
		if (NowSeconds - LastSinkWarningTime > 2.0)
		{
			UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Audio capture enabled but no active transport sink (transport=%s)."), *TransportName.ToString());
			LastSinkWarningTime = NowSeconds;
		}
	}
	else
	{
		LastSinkWarningTime = 0.0;
	}
}

void FO3DSenderAudioBinding::Detach(UO3DSenderAudioCaptureComponent* Capture)
{
	if (Capture)
	{
		Capture->SetAudioSink(nullptr, FString());
	}
	LastSinkWarningTime = 0.0;
}
