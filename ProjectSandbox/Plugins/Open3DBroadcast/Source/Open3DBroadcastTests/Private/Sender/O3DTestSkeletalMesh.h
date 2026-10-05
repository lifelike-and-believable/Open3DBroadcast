// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

// A skeletal mesh built in code for the sender tests (ADR 0008 Verification: the root-bone
// pose-equality test, WP-A2). The automation host project holds only the packaged plugin, so the
// tests cannot load a mesh asset; this builds one with a reference skeleton, a USkeleton, curve
// metadata and render data that has no geometry: the component evaluates bones and curves, and
// creates no mesh object for the renderer (USkinnedMeshComponent skips a LOD without vertices).

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "O3DTestSkeletalMesh.generated.h"

class USkeletalMesh;

namespace O3DTestSkeletalMesh
{
	/**
	 * A chain of NumBones bones ("bone_000" is the root, each next bone the child of the one before,
	 * 10 cm apart) and NumCurves curves ("curve_000"...) registered on its skeleton. Outer owns the
	 * mesh and its skeleton.
	 */
	USkeletalMesh* CreateChainMesh(UObject* Outer, int32 NumBones, int32 NumCurves);

	/** The root bone's X translation the root mover sets in the frame GFrameCounter == Frame. */
	inline double RootXForFrame(uint64 Frame)
	{
		return static_cast<double>(Frame % 100000ull);
	}
}

/**
 * Evaluates the reference pose with the root bone moved to X = RootXForFrame(GFrameCounter), so
 * each frame's evaluated pose is known in advance. A test that ticks a world advances
 * GFrameCounter before each tick (HANDOFF pitfall 28).
 */
struct FO3DRootMoverAnimInstanceProxy : public FAnimInstanceProxy
{
	explicit FO3DRootMoverAnimInstanceProxy(UAnimInstance* Instance)
		: FAnimInstanceProxy(Instance)
	{
	}

	virtual bool Evaluate(FPoseContext& Output) override;
};

UCLASS(Transient, NotBlueprintable)
class UO3DRootMoverAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override
	{
		return new FO3DRootMoverAnimInstanceProxy(this);
	}

	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override
	{
		delete static_cast<FO3DRootMoverAnimInstanceProxy*>(InProxy);
	}
};
