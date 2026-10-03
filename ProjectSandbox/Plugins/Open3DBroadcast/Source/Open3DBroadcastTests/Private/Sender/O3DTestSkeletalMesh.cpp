// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DTestSkeletalMesh.h"

#include "Animation/AnimNodeBase.h"
#include "Animation/Skeleton.h"
#include "BoneIndices.h"
#include "CoreGlobals.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace O3DTestSkeletalMesh
{
	USkeletalMesh* CreateChainMesh(UObject* Outer, int32 NumBones, int32 NumCurves)
	{
		check(NumBones > 0);

		USkeleton* Skeleton = NewObject<USkeleton>(Outer);
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Outer);
		{
			// The modifier rebuilds the reference skeleton when it goes out of scope.
			FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), Skeleton);
			for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
			{
				const FString BoneName = FString::Printf(TEXT("bone_%03d"), BoneIndex);
				Modifier.Add(FMeshBoneInfo(FName(*BoneName), BoneName, BoneIndex - 1), FTransform(FVector(0.0, 0.0, 10.0)));
			}
		}
		Mesh->SetSkeleton(Skeleton);
		Skeleton->MergeAllBonesToBoneTree(Mesh, false);
		Mesh->CalculateInvRefMatrices();
		Mesh->AddLODInfo();

		for (int32 CurveIndex = 0; CurveIndex < NumCurves; ++CurveIndex)
		{
			Skeleton->AddCurveMetaData(FName(*FString::Printf(TEXT("curve_%03d"), CurveIndex)), false);
		}

		// Render data with one LOD and no geometry: the LOD's bone lists are what the component
		// evaluates, and without vertices it creates no mesh object for the renderer.
		Mesh->AllocateResourceForRendering();
		FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		check(RenderData);
		TRefCountPtr<FSkeletalMeshLODRenderData> LODRenderData = MakeRefCount<FSkeletalMeshLODRenderData>();
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			LODRenderData->RequiredBones.Add(static_cast<FBoneIndexType>(BoneIndex));
			LODRenderData->ActiveBoneIndices.Add(static_cast<FBoneIndexType>(BoneIndex));
		}
		RenderData->LODRenderData.Add(LODRenderData);
		return Mesh;
	}
}

bool FO3DRootMoverAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	Output.ResetToRefPose();
	if (Output.Pose.GetNumBones() > 0)
	{
		FTransform& Root = Output.Pose[FCompactPoseBoneIndex(0)];
		Root.SetTranslation(FVector(O3DTestSkeletalMesh::RootXForFrame(GFrameCounter), 0.0, 0.0));
	}
	return true;
}
