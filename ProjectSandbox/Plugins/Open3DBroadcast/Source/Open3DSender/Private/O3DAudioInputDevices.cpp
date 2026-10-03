// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DAudioInputDevices.h"

#include "AudioCaptureCore.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "O3DSenderLogs.h"

static FAutoConsoleCommand GO3DSenderAudioRefreshDevicesCommand(
	TEXT("o3d.Sender.Audio.RefreshDevices"),
	TEXT("Enumerate the audio capture devices again and log them (the sender's device list is cached, ADR 0008 item 8)."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		FO3DAudioInputDevices& Devices = FO3DAudioInputDevices::Get();
		Devices.Refresh();
		const TArray<FName> Names = Devices.GetNames();
		UE_LOG(LogO3DSenderAudio, Display, TEXT("%d audio capture device(s):"), Names.Num());
		for (int32 Index = 0; Index < Names.Num(); ++Index)
		{
			UE_LOG(LogO3DSenderAudio, Display, TEXT("  [%d] %s"), Index, *Names[Index].ToString());
		}
	}));

FO3DAudioInputDevices& FO3DAudioInputDevices::Get()
{
	static FO3DAudioInputDevices Instance;
	return Instance;
}

TArray<FString> FO3DAudioInputDevices::EnumeratePlatformDevices()
{
	TArray<FString> Names;
	Audio::FAudioCapture Capture;
	TArray<Audio::FCaptureDeviceInfo> Devices;
	if (Capture.GetCaptureDevicesAvailable(Devices) > 0)
	{
		Names.Reserve(Devices.Num());
		for (const Audio::FCaptureDeviceInfo& Info : Devices)
		{
			Names.Add(Info.DeviceName);
		}
	}
	return Names;
}

void FO3DAudioInputDevices::Refresh()
{
	FEnumerator Enumerator;
	{
		FScopeLock Lock(&Mutex);
		Enumerator = TestEnumerator;
	}

	// Enumerate outside the lock: it talks to the platform.
	TArray<FString> Names = Enumerator ? Enumerator() : EnumeratePlatformDevices();

	FScopeLock Lock(&Mutex);
	DeviceNames = MoveTemp(Names);
	bHasEnumerated = true;
	++EnumerationCount;
}

TArray<FName> FO3DAudioInputDevices::GetNames() const
{
	FScopeLock Lock(&Mutex);
	TArray<FName> Names;
	Names.Reserve(DeviceNames.Num());
	for (const FString& Name : DeviceNames)
	{
		Names.Add(FName(*Name));
	}
	return Names;
}

int32 FO3DAudioInputDevices::FindIndex(FName DeviceName) const
{
	if (DeviceName.IsNone())
	{
		return -1;
	}

	const FString Wanted = DeviceName.ToString();
	FScopeLock Lock(&Mutex);
	for (int32 Index = 0; Index < DeviceNames.Num(); ++Index)
	{
		if (DeviceNames[Index].Equals(Wanted, ESearchCase::IgnoreCase))
		{
			return Index;
		}
	}
	return -1;
}

bool FO3DAudioInputDevices::HasEnumerated() const
{
	FScopeLock Lock(&Mutex);
	return bHasEnumerated;
}

int32 FO3DAudioInputDevices::GetEnumerationCount() const
{
	FScopeLock Lock(&Mutex);
	return EnumerationCount;
}

void FO3DAudioInputDevices::SetEnumeratorForTesting(FEnumerator InEnumerator)
{
	FScopeLock Lock(&Mutex);
	TestEnumerator = MoveTemp(InEnumerator);
}
