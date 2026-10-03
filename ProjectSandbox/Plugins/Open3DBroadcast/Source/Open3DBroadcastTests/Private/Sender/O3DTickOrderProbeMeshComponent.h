// Copyright Lifelike & Believable. All Rights Reserved.

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

	// Super::TickComponent is not called: with no skeletal mesh asset the base tick stopped the
	// component ticking after the first frame in CI (#317). The test checks how the tick
	// prerequisite orders the two tick functions, which does not depend on the mesh's own tick work.
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override
	{
		if (OnTicked)
		{
			OnTicked();
		}
	}

	/** Called on each tick of the probe. */
	TFunction<void()> OnTicked;
};
