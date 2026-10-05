// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

// A skeletal mesh component that reports each of its ticks, for the sender tick-order test
// (ADR 0008 item 9, WP-A2b). It ticks in the sender's own group, TG_PostUpdateWork, so only the
// tick prerequisite the sender adds can order the two.

#include "CoreMinimal.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/EngineBaseTypes.h"
#include "Templates/Function.h"
#include "O3DTickOrderProbeMeshComponent.generated.h"

UCLASS()
class UO3DTickOrderProbeMeshComponent : public USkeletalMeshComponent
{
	GENERATED_BODY()

public:
	UO3DTickOrderProbeMeshComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get())
		: Super(ObjectInitializer)
	{
		PrimaryComponentTick.bCanEverTick = true;
		PrimaryComponentTick.bStartWithTickEnabled = true;
		PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	}

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override
	{
		Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
		if (OnTicked)
		{
			OnTicked();
		}
	}

	/** Called at the end of each tick, after the mesh's own tick work. */
	TFunction<void()> OnTicked;
};
