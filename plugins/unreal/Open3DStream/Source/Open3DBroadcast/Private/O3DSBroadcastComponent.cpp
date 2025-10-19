// Copyright (c) Open3DStream Contributors

#include "O3DSBroadcastComponent.h"
#include "Open3DBroadcast.h" // for LogO3DSBroadcast category declaration
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "AnimationRuntime.h"
#include "Components/SkinnedMeshComponent.h"
#include "HAL/IConsoleManager.h"

// CVar to toggle verbose debug logging
static TAutoConsoleVariable<int32> CVarO3DSBroadcastDebugPose(
    TEXT("o3ds.Broadcast.DebugPose"),
    0,
    TEXT("Enable per-frame pose debug logging for Open3DBroadcast (0/1)."),
    ECVF_Default);

UO3DSBroadcastComponent::UO3DSBroadcastComponent()
{
    PrimaryComponentTick.bCanEverTick = true; // we'll manually enable when capturing
    SetComponentTickEnabled(false);
}

void UO3DSBroadcastComponent::BeginPlay()
{
    Super::BeginPlay();

    if (!TargetMesh.IsValid())
    {
        // Auto-discover first skeletal mesh on owner
        if (AActor* Owner = GetOwner())
        {
            TargetMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
        }
    }

    // Do not start by default; StartCapture can be called from code/editor later
}

void UO3DSBroadcastComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopCapture();
    Super::EndPlay(EndPlayReason);
}

void UO3DSBroadcastComponent::StartCapture()
{
    if (bIsCapturing)
    {
        return;
    }
    BindToTarget();
    bIsCapturing = TargetMesh.IsValid();
    LastCaptureTime = 0.0;
    FrameCounter = 0;
    SetComponentTickEnabled(bIsCapturing);
}

void UO3DSBroadcastComponent::StopCapture()
{
    if (!bIsCapturing)
    {
        return;
    }
    UnbindFromTarget();
    bIsCapturing = false;
    SetComponentTickEnabled(false);
}

void UO3DSBroadcastComponent::BindToTarget()
{
    if (!TargetMesh.IsValid())
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("No TargetMesh set for UO3DSBroadcastComponent on %s"), *GetNameSafe(GetOwner()));
        return;
    }
    EnsureSkeletonCache(TargetMesh.Get());
    UE_LOG(LogO3DSBroadcast, Log, TEXT("Broadcast capture bound to %s"), *GetNameSafe(TargetMesh.Get()));
}

void UO3DSBroadcastComponent::UnbindFromTarget()
{
    if (USkinnedMeshComponent* Skinned = TargetMesh.Get())
    {
        UE_LOG(LogO3DSBroadcast, Log, TEXT("Broadcast capture unbound from %s"), *GetNameSafe(Skinned));
    }
}

void UO3DSBroadcastComponent::EnsureSkeletonCache(USkeletalMeshComponent* SkelComp)
{
    if (!SkelComp)
    {
        return;
    }
    USkeletalMesh* Mesh = SkelComp->GetSkeletalMeshAsset();
    USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;

    if (CachedSkeletalMesh.Get() != Mesh || CachedSkeleton.Get() != Skeleton)
    {
        RefreshSkeletonCache(SkelComp);
    }
}

void UO3DSBroadcastComponent::RefreshSkeletonCache(USkeletalMeshComponent* SkelComp)
{
    BoneNames.Reset();
    ParentIndices.Reset();

    USkeletalMesh* Mesh = SkelComp ? SkelComp->GetSkeletalMeshAsset() : nullptr;
    USkeleton* Skeleton = Mesh ? Mesh->GetSkeleton() : nullptr;
    CachedSkeletalMesh = Mesh;
    CachedSkeleton = Skeleton;

    if (!Skeleton)
    {
        return;
    }

    const FReferenceSkeleton& RefSkel = Skeleton->GetReferenceSkeleton();
    const int32 NumBones = RefSkel.GetNum();
    BoneNames.Reserve(NumBones);
    ParentIndices.Reserve(NumBones);

    for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
    {
        BoneNames.Add(RefSkel.GetBoneName(BoneIndex));
        ParentIndices.Add(RefSkel.GetParentIndex(BoneIndex));
    }

    UE_LOG(LogO3DSBroadcast, Log, TEXT("Cached skeleton for %s: %d bones"), *GetNameSafe(SkelComp), NumBones);
}

FString UO3DSBroadcastComponent::BuildSubjectName(const USkeletalMeshComponent* SkelComp) const
{
    const UWorld* World = SkelComp ? SkelComp->GetWorld() : nullptr;
    const FString WorldName = World ? World->GetName() : TEXT("World");
    const FString ActorName = SkelComp && SkelComp->GetOwner() ? SkelComp->GetOwner()->GetName() : TEXT("Actor");
    const FString CompName = SkelComp ? SkelComp->GetName() : TEXT("SkeletalMeshComponent");
    return FString::Printf(TEXT("%s/%s/%s"), *WorldName, *ActorName, *CompName);
}

void UO3DSBroadcastComponent::HandleBoneTransformsFinalized(USkinnedMeshComponent* SkinnedMesh, bool /*bRequiredBonesOnly*/)
{
    if (!bIsCapturing)
    {
        return;
    }

    USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(SkinnedMesh);
    if (!SkelComp)
    {
        return;
    }

    // Only process events from our target mesh
    if (TargetMesh.Get() != SkelComp)
    {
        return;
    }

    EnsureSkeletonCache(SkelComp);

    // Optional rate limiting
    if (CaptureRateHz > 0.0f)
    {
        const double Now = FPlatformTime::Seconds();
        const double MinDelta = 1.0 / FMath::Max(1.0f, CaptureRateHz);
        if ((Now - LastCaptureTime) < MinDelta)
        {
            return;
        }
        LastCaptureTime = Now;
    }

    const TArray<FTransform>& CompSpace = SkelComp->GetComponentSpaceTransforms();
    const int32 Count = CompSpace.Num();

    // Guard: ensure names/parents align length-wise; if mismatch, refresh cache and clamp
    if (Count != BoneNames.Num())
    {
        RefreshSkeletonCache(SkelComp);
    }

    const int32 N = FMath::Min(Count, BoneNames.Num());
    const FString Subject = BuildSubjectName(SkelComp);

    const bool bDebug = (CVarO3DSBroadcastDebugPose.GetValueOnAnyThread() != 0);
    if (bDebug)
    {
        UE_LOG(LogO3DSBroadcast, Log, TEXT("[O3DS] Pose #%llu Subject=%s Bones=%d"), (unsigned long long)++FrameCounter, *Subject, N);
    }

    // Log first few bones for verification (component-space parent-relative per M0.3)
    const int32 LogCount = bDebug ? FMath::Min(5, N) : 0;
    for (int32 i = 0; i < N; ++i)
    {
        const int32 ParentIdx = (i < ParentIndices.Num()) ? ParentIndices[i] : INDEX_NONE;
        const FTransform& ThisCS = CompSpace[i];
        FTransform Rel;
        if (ParentIdx >= 0 && ParentIdx < CompSpace.Num())
        {
            Rel = ThisCS.GetRelativeTransform(CompSpace[ParentIdx]);
        }
        else
        {
            Rel = ThisCS; // root relative to component origin
        }

        if (i < LogCount)
        {
            const FVector T = Rel.GetTranslation();
            const FQuat Q = Rel.GetRotation().GetNormalized();
            const FVector S = Rel.GetScale3D();
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("  [%d] %s p=%d T(%.2f,%.2f,%.2f) Q(%.3f,%.3f,%.3f,%.3f) S(%.2f,%.2f,%.2f)"),
                i, *BoneNames[i].ToString(), ParentIdx, T.X, T.Y, T.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z);
        }
    }
}

void UO3DSBroadcastComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bIsCapturing || !TargetMesh.IsValid())
    {
        return;
    }

    // We depend on animation being evaluated earlier in the frame; use current evaluated transforms
    HandleBoneTransformsFinalized(TargetMesh.Get(), /*bRequiredBonesOnly*/ false);
}
