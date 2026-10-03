// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSenderPoseSampler.h"

#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "O3DHelpers.h"
#include "O3DSenderLogs.h"

void FO3DSenderPoseSampler::SetCallbacks(FDescriptorChanged InOnDescriptorChanged, FSubjectNameChanged InOnSubjectNameChanged)
{
	OnDescriptorChanged = MoveTemp(InOnDescriptorChanged);
	OnSubjectNameChanged = MoveTemp(InOnSubjectNameChanged);
}

bool FO3DSenderPoseSampler::EnsureSkeleton(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, bool bDebugLog)
{
	if (!Mesh)
	{
		return false;
	}

	USkeletalMesh* Asset = Mesh->GetSkeletalMeshAsset();
	USkeleton* Skeleton = Asset ? Asset->GetSkeleton() : nullptr;
	const FName CurrentMeshName = Asset ? Asset->GetFName() : NAME_None;
	if (CachedSkeletalMesh.Get() == Asset && CachedSkeleton.Get() == Skeleton && CachedSkeletalMeshName == CurrentMeshName)
	{
		return false;
	}
	RefreshSkeleton(Mesh, SubjectOverride, bDebugLog);
	return true;
}

void FO3DSenderPoseSampler::RefreshSkeleton(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, bool bDebugLog)
{
	USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	CachedSkeletalMesh = Asset;
	CachedSkeleton = Asset ? Asset->GetSkeleton() : nullptr;
	CachedSkeletalMeshName = Asset ? Asset->GetFName() : NAME_None;

	if (!Asset)
	{
		Descriptor.Reset();
		DescriptorSnapshot.Reset();
		bDescriptorDirty = true;
		return;
	}

	const uint64 PreviousHash = Descriptor.Hash;
	const int32 PreviousCount = Descriptor.BoneNames.Num();

	Descriptor.BoneNames.Reset();
	Descriptor.ParentIndices.Reset();

	const FReferenceSkeleton& RefSkel = Asset->GetRefSkeleton();
	const int32 NumBones = RefSkel.GetNum();
	Descriptor.BoneNames.Reserve(NumBones);
	Descriptor.ParentIndices.Reserve(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		Descriptor.BoneNames.Add(RefSkel.GetBoneName(BoneIndex));
		Descriptor.ParentIndices.Add(RefSkel.GetParentIndex(BoneIndex));
	}

	const uint64 NewHash = O3DHelpers::HashNamesAndParents(Descriptor.BoneNames, Descriptor.ParentIndices);
	const bool bChanged = (PreviousHash != NewHash) || (PreviousCount != Descriptor.BoneNames.Num());
	Descriptor.Hash = NewHash;
	bDescriptorDirty = bChanged;
	DescriptorSnapshot = MakeShared<FO3DSSkeletonDescriptor>(Descriptor);

	if (bDebugLog)
	{
		UE_LOG(LogO3DSenderComponent, Log, TEXT("Cached skeleton for %s: %d bones, Hash=0x%llx%s"),
			*GetNameSafe(Mesh), NumBones, (unsigned long long)Descriptor.Hash, bDescriptorDirty ? TEXT(" [Changed]") : TEXT(""));
	}

	if (bDescriptorDirty)
	{
		// Named for the mesh as it is now, not the cached subject name.
		if (OnDescriptorChanged)
		{
			OnDescriptorChanged(BuildSubjectName(Mesh, SubjectOverride), Descriptor);
		}
		bDescriptorDirty = false;
	}
}

void FO3DSenderPoseSampler::ResetSkeleton()
{
	CachedSkeletalMesh.Reset();
	CachedSkeleton.Reset();
	CachedSkeletalMeshName = NAME_None;
	Descriptor.Reset();
	DescriptorSnapshot.Reset();
	bDescriptorDirty = false;
}

void FO3DSenderPoseSampler::SetDescriptor(const FO3DSSkeletonDescriptor& InDescriptor)
{
	Descriptor = InDescriptor;
	DescriptorSnapshot = MakeShared<FO3DSSkeletonDescriptor>(InDescriptor);
}

FString FO3DSenderPoseSampler::BuildSubjectName(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride)
{
	if (!SubjectOverride.IsEmpty())
	{
		return O3DHelpers::SanitizeSubjectName(SubjectOverride);
	}

	const UWorld* World = Mesh ? Mesh->GetWorld() : nullptr;
	const FString WorldName = World ? World->GetName() : TEXT("World");
	const FString ActorName = Mesh && Mesh->GetOwner() ? Mesh->GetOwner()->GetName() : TEXT("Actor");
	const FString CompName = Mesh ? Mesh->GetName() : TEXT("SkeletalMeshComponent");
	return O3DHelpers::SanitizeSubjectName(FString::Printf(TEXT("%s/%s/%s"), *WorldName, *ActorName, *CompName));
}

const FString& FO3DSenderPoseSampler::ResolveSubjectName(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride)
{
	USkeletalMesh* Asset = Mesh ? Mesh->GetSkeletalMeshAsset() : nullptr;
	const bool bOverrideChanged = (LastSubjectOverride != SubjectOverride);
	const bool bMeshChanged = SubjectNameMesh.Get() != Asset;
	if (!bOverrideChanged && !bMeshChanged && !SubjectName.IsEmpty())
	{
		return SubjectName;
	}

	const FString PreviousName = SubjectName;
	SubjectName = BuildSubjectName(Mesh, SubjectOverride);
	SubjectNameMesh = Asset;
	LastSubjectOverride = SubjectOverride;

	if (!PreviousName.Equals(SubjectName, ESearchCase::CaseSensitive) && OnSubjectNameChanged)
	{
		OnSubjectNameChanged(PreviousName, SubjectName);
	}
	return SubjectName;
}

FString FO3DSenderPoseSampler::InvalidateSubjectName(const FString& SubjectOverride)
{
	FString PreviousName = MoveTemp(SubjectName);
	SubjectName.Reset();
	SubjectNameMesh.Reset();
	LastSubjectOverride = SubjectOverride;
	return PreviousName;
}

void FO3DSenderPoseSampler::FillShell(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, double CaptureTimeSec,
	const FO3DSenderEncodingSettings& Encoding, FO3DSPoseFrame& Frame)
{
	Frame.Subject = ResolveSubjectName(Mesh, SubjectOverride);
	Frame.FrameIndex = ++FrameCounter;
	// ADR 0005 (i): the frame carries the descriptor it was sampled against,
	// so the serializer never depends on having seen OnDescriptorReady.
	Frame.Descriptor = DescriptorSnapshot;
	// ADR 0008 item 7: the sampling time, which is also the wire time.
	Frame.CaptureTimeSec = CaptureTimeSec;
	Frame.Encoding = Encoding;
}

void FO3DSenderPoseSampler::BuildLocalBoneTransforms(const TArray<FTransform>& ComponentSpaceTransforms,
	const TArray<int32>& CachedParentIndices,
	int32 NumBones,
	TFunctionRef<int32(int32)> ResolveFallbackParent,
	TArray<FTransform>& OutLocalTransforms,
	TArray<int32>* OutResolvedParents)
{
	if (NumBones <= 0)
	{
		OutLocalTransforms.Reset();
		if (OutResolvedParents)
		{
			OutResolvedParents->Reset();
		}
		return;
	}

	OutLocalTransforms.SetNum(NumBones, EAllowShrinking::No);
	if (OutResolvedParents)
	{
		OutResolvedParents->SetNum(NumBones, EAllowShrinking::No);
	}

	const int32 TransformCount = ComponentSpaceTransforms.Num();
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		int32 ParentIndex = (BoneIndex >= 0 && BoneIndex < CachedParentIndices.Num()) ? CachedParentIndices[BoneIndex] : INDEX_NONE;
		if (ParentIndex < 0 || ParentIndex >= TransformCount)
		{
			ParentIndex = ResolveFallbackParent(BoneIndex);
		}
		if (ParentIndex < 0 || ParentIndex >= TransformCount)
		{
			ParentIndex = INDEX_NONE;
		}

		if (OutResolvedParents)
		{
			(*OutResolvedParents)[BoneIndex] = ParentIndex;
		}

		const FTransform& ComponentTransform = ComponentSpaceTransforms[BoneIndex];
		FTransform Relative = ComponentTransform;
		if (ParentIndex != INDEX_NONE)
		{
			Relative = ComponentTransform.GetRelativeTransform(ComponentSpaceTransforms[ParentIndex]);
		}

		FQuat Rotation = Relative.GetRotation();
		if (!Rotation.IsNormalized())
		{
			Rotation.Normalize();
			Relative.SetRotation(Rotation);
		}

		OutLocalTransforms[BoneIndex] = Relative;
	}
}

void FO3DSenderPoseSampler::SampleBones(const USkeletalMeshComponent* Mesh, FO3DSPoseFrame& Frame, bool bDebugLog) const
{
	if (!Mesh)
	{
		Frame.BoneLocalTransforms.Reset();
		return;
	}

	const TArray<FTransform>& ComponentSpace = Mesh->GetComponentSpaceTransforms();
	const TArray<FName>& CachedBoneNames = Descriptor.BoneNames;
	const int32 NumBones = FMath::Min(ComponentSpace.Num(), CachedBoneNames.Num());
	if (NumBones <= 0)
	{
		Frame.BoneLocalTransforms.Reset();
		return;
	}

	TArray<int32> ResolvedParents;
	const auto ResolveFallbackParent = [Mesh, &CachedBoneNames](int32 BoneIndex) -> int32
	{
		const FName BoneName = CachedBoneNames.IsValidIndex(BoneIndex) ? CachedBoneNames[BoneIndex] : NAME_None;
		if (BoneName == NAME_None)
		{
			return INDEX_NONE;
		}
		const FName ParentBoneName = Mesh->GetParentBone(BoneName);
		return ParentBoneName == NAME_None ? INDEX_NONE : Mesh->GetBoneIndex(ParentBoneName);
	};
	BuildLocalBoneTransforms(ComponentSpace, Descriptor.ParentIndices, NumBones, ResolveFallbackParent, Frame.BoneLocalTransforms,
		bDebugLog ? &ResolvedParents : nullptr);

	if (bDebugLog)
	{
		const int32 DebugCount = FMath::Min(NumBones, 5);
		for (int32 BoneIndex = 0; BoneIndex < DebugCount; ++BoneIndex)
		{
			const FTransform& Relative = Frame.BoneLocalTransforms[BoneIndex];
			const FVector Translation = Relative.GetTranslation();
			const FVector Scale = Relative.GetScale3D();
			UE_LOG(LogO3DSenderComponent, Verbose, TEXT("[%d] %s Parent=%d Pos(%.2f,%.2f,%.2f) Scale(%.2f,%.2f,%.2f)"),
				BoneIndex,
				*CachedBoneNames[BoneIndex].ToString(),
				ResolvedParents[BoneIndex],
				Translation.X, Translation.Y, Translation.Z,
				Scale.X, Scale.Y, Scale.Z);
		}
	}
}
