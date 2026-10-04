// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/Function.h"

/**
 * Process-wide cache of the audio capture devices (ADR 0008 item 8, SND-18). Enumerating devices
 * creates an Audio::FAudioCapture and asks the platform, so it runs only:
 * - once per UO3DSenderComponent::StartCapture that captures from an input device;
 * - once in the editor after engine init, so the device pickers have a list;
 * - when Refresh is called (UO3DSenderComponent::RefreshAudioInputDevices, console command
 *   o3d.Sender.Audio.RefreshDevices).
 * Lookups (the device pickers' GetOptions functions, name-to-index resolution) read the cache and
 * never enumerate. Any thread; the lock is held only to copy or swap the list.
 *
 * Process-wide by nature, so not part of FO3DRuntimeContext (docs/adr/0012-runtime-services-and-global-state.md,
 * item 1): it describes the machine's hardware. See docs/dev/runtime-services.md.
 */
class OPEN3DSENDER_API FO3DAudioInputDevices
{
public:
	/** Returns the device names in the platform's order; the index is the capture device index. */
	using FEnumerator = TFunction<TArray<FString>()>;

	static FO3DAudioInputDevices& Get();

	/** Enumerate the devices now and replace the cached list. */
	void Refresh();

	/** The cached device names (empty until the first Refresh). */
	TArray<FName> GetNames() const;

	/** The cached index of the device with this name (case-insensitive); -1 for None or not found. */
	int32 FindIndex(FName DeviceName) const;

	/** True once a Refresh has run. */
	bool HasEnumerated() const;

	/** Number of enumerations since startup (tests, stats). */
	int32 GetEnumerationCount() const;

	/** Tests only: replace the platform enumeration; an empty function restores it. */
	void SetEnumeratorForTesting(FEnumerator InEnumerator);

private:
	static TArray<FString> EnumeratePlatformDevices();

	mutable FCriticalSection Mutex;
	TArray<FString> DeviceNames;
	FEnumerator TestEnumerator;
	int32 EnumerationCount = 0;
	bool bHasEnumerated = false;
};
