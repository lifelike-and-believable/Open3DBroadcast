// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DControlSettings.h"

#include "O3DControlBus.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/parse_limits.h"
THIRD_PARTY_INCLUDES_END

// The defaults above are the core receiver's defaults (src/o3ds/parse_limits.h).
static_assert(64 * 1024 == static_cast<int32>(O3DS::ControlLimits::kReceiverDefaultLiveBytesPerS), "keep MaxControlLiveBytesPerSecond's default in step with the core");
static_assert(512 * 1024 == static_cast<int32>(O3DS::ControlLimits::kReceiverDefaultSnapshotBytesPerS), "keep MaxControlSnapshotBytesPerSecond's default in step with the core");
static_assert(1024 == O3DS::ControlLimits::kMaxKeysPerSource, "keep MaxControlKeysPerSource's default and clamp in step with the core");

UO3DControlSettings::UO3DControlSettings()
{
	CategoryName = TEXT("Plugins");
}

bool UO3DControlSettings::IsReceiveEnabled(EO3DControlAcceptMode PerSource)
{
	switch (PerSource)
	{
	case EO3DControlAcceptMode::Enabled: return true;
	case EO3DControlAcceptMode::Disabled: return false;
	case EO3DControlAcceptMode::ProjectDefault: break;
	}
	const TOptional<bool> Override = FO3DControlBus::GetReceiveOverride();
	if (Override.IsSet())
	{
		return Override.GetValue();
	}
	return GetDefault<UO3DControlSettings>()->bAcceptControl;
}

void UO3DControlLibrary::SetControlReceiveEnabled(bool bEnabled)
{
	FO3DControlBus::SetReceiveOverride(bEnabled);
}

void UO3DControlLibrary::ClearControlReceiveOverride()
{
	FO3DControlBus::SetReceiveOverride(TOptional<bool>());
}

bool UO3DControlLibrary::IsControlReceiveEnabled()
{
	return UO3DControlSettings::IsReceiveEnabled(EO3DControlAcceptMode::ProjectDefault);
}
