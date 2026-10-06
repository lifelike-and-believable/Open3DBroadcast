// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

// Records what a UO3DSenderComponent broadcasts to Blueprint (WP-U2). Dynamic delegates bind only
// to UFUNCTIONs on UObjects, so tests use this instead of a lambda.

#include "CoreMinimal.h"
#include "O3DBlueprintTransportTypes.h"
#include "UObject/Object.h"
#include "O3DSenderTestListener.generated.h"

UCLASS()
class UO3DSenderTestListener : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnStateChanged(EO3DBroadcastConnectionState NewState) { States.Add(NewState); }

	UFUNCTION()
	void OnStarted() { ++Started; }

	UFUNCTION()
	void OnStopped() { ++Stopped; }

	UFUNCTION()
	void OnError(const FString& Message) { Errors.Add(Message); }

	TArray<EO3DBroadcastConnectionState> States;
	TArray<FString> Errors;
	int32 Started = 0;
	int32 Stopped = 0;
};
