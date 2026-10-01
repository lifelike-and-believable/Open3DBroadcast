// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

// Records what a UO3DRemoteControlComponent broadcasts. Dynamic delegates bind only to UFUNCTIONs
// on UObjects, so tests use this instead of a lambda (ADR 0011, CTL-4).

#include "CoreMinimal.h"
#include "O3DControlTypes.h"
#include "UObject/Object.h"
#include "O3DControlTestListener.generated.h"

UCLASS()
class UO3DControlTestListener : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnEvent(const FString& EventName, const FO3DControlValue& Value, const FO3DControlMeta& Meta)
	{
		Events.Add(EventName);
		LastEventValue = Value;
		LastEventMeta = Meta;
	}

	UFUNCTION()
	void OnValueChanged(const FString& Key, const FO3DControlValue& Value, const FO3DControlMeta& Meta)
	{
		Changed.Add(Key);
		LastChangedValue = Value;
	}

	UFUNCTION()
	void OnValueCleared(const FString& Key, const FO3DControlMeta& Meta)
	{
		Cleared.Add(Key);
	}

	TArray<FString> Events;
	TArray<FString> Changed;
	TArray<FString> Cleared;
	FO3DControlValue LastEventValue;
	FO3DControlValue LastChangedValue;
	FO3DControlMeta LastEventMeta;
};
