// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DSenderComponent.h"
#include "Templates/Function.h"

class USkeletalMeshComponent;
class USkeletalMesh;
class USkeleton;

/**
 * The sender's pose sampling (WP-A3 step 6, SND-22): the skeleton descriptor cache, the subject
 * name, and filling a sampled frame's shell and bones from the target mesh. Game thread only, like
 * the component that owns it. Everything that reaches outside (the OnDescriptorReady delegate, the
 * audio stream label, the pipeline's serializer state) goes through the two callbacks.
 */
class FO3DSenderPoseSampler
{
public:
	/** A refresh changed the skeleton: the subject name built for the mesh, and the new descriptor. */
	using FDescriptorChanged = TFunction<void(const FString& Subject, const FO3DSSkeletonDescriptor& Descriptor)>;
	/** The cached subject name changed (case-sensitive): the previous name (empty the first time) and the new one. */
	using FSubjectNameChanged = TFunction<void(const FString& Previous, const FString& Current)>;

	void SetCallbacks(FDescriptorChanged InOnDescriptorChanged, FSubjectNameChanged InOnSubjectNameChanged);

	/**
	 * Rebuilds the descriptor when the mesh asset, its skeleton or its name changed since the last
	 * build. True when it rebuilt (the curve cache depends on the mesh too).
	 */
	bool EnsureSkeleton(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, bool bDebugLog);
	/** Forgets the skeleton, so the next EnsureSkeleton rebuilds and reports it (SND-1). */
	void ResetSkeleton();

	/** The override (sanitized) or "World/Actor/Component" for the mesh; placeholders without one. */
	static FString BuildSubjectName(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride);
	/** The cached subject name, rebuilt when the override or the mesh asset changed. */
	const FString& ResolveSubjectName(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride);
	/** Clears the cached name; returns the name that was cached, so the caller can forget it downstream. */
	FString InvalidateSubjectName(const FString& SubjectOverride);
	const FString& GetSubjectName() const { return SubjectName; }

	void ResetFrameCounter() { FrameCounter = 0; }
	/** The frame's subject, index, descriptor snapshot, sampling time and settings snapshot. */
	void FillShell(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, double CaptureTimeSec,
		const FO3DSenderEncodingSettings& Encoding, FO3DSPoseFrame& Frame);
	/** The mesh's bones in parent space, against the cached descriptor. */
	void SampleBones(const USkeletalMeshComponent* Mesh, FO3DSPoseFrame& Frame, bool bDebugLog) const;

	/**
	 * Converts component-space transforms to parent space. A parent outside the transforms is
	 * resolved through ResolveFallbackParent; still outside, the bone is a root.
	 */
	static void BuildLocalBoneTransforms(const TArray<FTransform>& ComponentSpaceTransforms,
		const TArray<int32>& CachedParentIndices,
		int32 NumBones,
		TFunctionRef<int32(int32)> ResolveFallbackParent,
		TArray<FTransform>& OutLocalTransforms,
		TArray<int32>* OutResolvedParents);

	const FO3DSSkeletonDescriptor& GetDescriptor() const { return Descriptor; }
	bool HasDescriptorSnapshot() const { return DescriptorSnapshot.IsValid(); }
	/** Tests: replaces the descriptor and its snapshot as if a mesh with this skeleton had been cached. */
	void SetDescriptor(const FO3DSSkeletonDescriptor& InDescriptor);

private:
	void RefreshSkeleton(const USkeletalMeshComponent* Mesh, const FString& SubjectOverride, bool bDebugLog);

	FDescriptorChanged OnDescriptorChanged;
	FSubjectNameChanged OnSubjectNameChanged;

	TWeakObjectPtr<USkeleton> CachedSkeleton;
	TWeakObjectPtr<USkeletalMesh> CachedSkeletalMesh;
	FName CachedSkeletalMeshName = NAME_None;
	FO3DSSkeletonDescriptor Descriptor;
	/** Immutable copy of Descriptor attached to every sampled frame (ADR 0005 (i)). */
	TSharedPtr<const FO3DSSkeletonDescriptor> DescriptorSnapshot;
	bool bDescriptorDirty = false;

	FString SubjectName;
	FString LastSubjectOverride;
	TWeakObjectPtr<USkeletalMesh> SubjectNameMesh;

	uint64 FrameCounter = 0;
};
