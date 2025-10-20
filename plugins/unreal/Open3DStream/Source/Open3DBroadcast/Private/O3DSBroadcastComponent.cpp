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
#include "Animation/AnimInstance.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/MorphTarget.h"

// CVar to toggle verbose debug logging
static TAutoConsoleVariable<int32> CVarO3DSBroadcastDebugPose(
    TEXT("o3ds.Broadcast.DebugPose"),
    0,
    TEXT("Enable per-frame pose debug logging for Open3DBroadcast (0/1)."),
    ECVF_Default);

// CVar to toggle curve debug logging
static TAutoConsoleVariable<int32> CVarO3DSBroadcastDebugCurves(
    TEXT("o3ds.Broadcast.DebugCurves"),
    0,
    TEXT("Enable per-frame curve debug logging for Open3DBroadcast (0/1)."),
    ECVF_Default);

UO3DSBroadcastComponent::UO3DSBroadcastComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // delegate-driven capture, no tick needed
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
}

void UO3DSBroadcastComponent::StopCapture()
{
    if (!bIsCapturing)
    {
        return;
    }
    UnbindFromTarget();
    bIsCapturing = false;
}

void UO3DSBroadcastComponent::BindToTarget()
{
    if (!TargetMesh.IsValid())
    {
        UE_LOG(LogO3DSBroadcast, Warning, TEXT("No TargetMesh set for UO3DSBroadcastComponent on %s"), *GetNameSafe(GetOwner()));
        return;
    }
    EnsureSkeletonCache(TargetMesh.Get());
    // Bind to post-evaluation delegate so we sample after the pose is finalized
    if (USkinnedMeshComponent* Skinned = TargetMesh.Get())
    {
        if (!BoneTransformsFinalizedHandle.IsValid())
        {
            // Use virtual registration to be safe against base pointer (USkinnedMeshComponent) in UE 5.6
            BoneTransformsFinalizedHandle = Skinned->RegisterOnBoneTransformsFinalizedDelegate(
                FOnBoneTransformsFinalizedMultiCast::FDelegate::CreateUObject(this, &UO3DSBroadcastComponent::HandleBoneTransformsFinalized));
        }
    }
    UE_LOG(LogO3DSBroadcast, Log, TEXT("Broadcast capture bound to %s"), *GetNameSafe(TargetMesh.Get()));
}

void UO3DSBroadcastComponent::UnbindFromTarget()
{
    if (USkinnedMeshComponent* Skinned = TargetMesh.Get())
    {
        if (BoneTransformsFinalizedHandle.IsValid())
        {
            Skinned->UnregisterOnBoneTransformsFinalizedDelegate(BoneTransformsFinalizedHandle);
            BoneTransformsFinalizedHandle.Reset();
        }
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
        // Also refresh curve caches on mesh/skeleton change
        bCurveCacheInitialized = false;
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
    const FString Raw = FString::Printf(TEXT("%s/%s/%s"), *WorldName, *ActorName, *CompName);
    return SanitizeSubjectName(Raw);
}

FString UO3DSBroadcastComponent::SanitizeSubjectName(const FString& Raw) const
{
    // Replace whitespace with underscore
    FString Out = Raw;
    Out = Out.Replace(TEXT(" "), TEXT("_"));

    // Remove characters not in [-._A-Za-z0-9/]
    // Build allowed set and remove others by iterating
    FString Result;
    Result.Reserve(Out.Len());
    for (int32 i = 0; i < Out.Len(); ++i)
    {
        TCHAR C = Out[i];
        bool bAllow = false;
        if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'))
            bAllow = true;
        else if (C == '-' || C == '.' || C == '_' || C == '/')
            bAllow = true;

        if (bAllow)
            Result.AppendChar(C);
        // else drop
    }
    return Result;
}

void UO3DSBroadcastComponent::EnsureCurveCache(USkeletalMeshComponent* SkelComp)
{
    if (!bCurveCacheInitialized)
    {
        RefreshCurveCache(SkelComp);
    }
}

void UO3DSBroadcastComponent::RefreshCurveCache(USkeletalMeshComponent* SkelComp)
{
    CurveNames.Reset();
    CurveValues.Reset();
    MorphNameSet.Reset();
    CurveNameSet.Reset();

    if (!SkelComp)
    {
        bCurveCacheInitialized = true;
        return;
    }

    // 1) Morph targets from skeletal mesh
    if (USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset())
    {
        const TArray<UMorphTarget*>& Morphs = SkelMesh->GetMorphTargets();
        CurveNames.Reserve(CurveNames.Num() + Morphs.Num());
        for (UMorphTarget* MT : Morphs)
        {
            if (!MT) continue;
            const FName Name = MT->GetFName();
            if (!CurveNameSet.Contains(Name))
            {
                MorphNameSet.Add(Name);
                CurveNames.Add(Name);
                CurveNameSet.Add(Name);
            }
        }
    }

    // 2) Named animation curves — preferred, forward-compatible path: query from evaluated AnimInstance
    if (SkelComp)
    {
        UAnimInstance* SourceAnim = SkelComp->GetPostProcessInstance();
        if (!SourceAnim)
        {
            SourceAnim = SkelComp->GetAnimInstance();
        }

        if (SourceAnim)
        {
            // Collect attribute/material/morph curve names known to this instance
            TArray<FName> Names;

            // Attribute curves
            {
                TMap<FName, float> AttrCurves;
                SourceAnim->GetAnimationCurveList(EAnimCurveType::AttributeCurve, AttrCurves);
                if (!AttrCurves.IsEmpty())
                {
                    TArray<FName> AttrKeys;
                    AttrCurves.GetKeys(AttrKeys);
                    Names.Append(AttrKeys);
                }
            }

            // Material curves (if any)
            {
                TMap<FName, float> MaterialCurves;
                SourceAnim->GetAnimationCurveList(EAnimCurveType::MaterialCurve, MaterialCurves);
                if (!MaterialCurves.IsEmpty())
                {
                    TArray<FName> MaterialKeys;
                    MaterialCurves.GetKeys(MaterialKeys);
                    Names.Append(MaterialKeys);
                }
            }

            // Morph target curves (when authored as anim curves)
            {
                TMap<FName, float> MorphCurves;
                SourceAnim->GetAnimationCurveList(EAnimCurveType::MorphTargetCurve, MorphCurves);
                if (!MorphCurves.IsEmpty())
                {
                    TArray<FName> MorphKeys;
                    MorphCurves.GetKeys(MorphKeys);
                    Names.Append(MorphKeys);
                }
            }

            CurveNames.Reserve(CurveNames.Num() + Names.Num());
            for (const FName& N : Names)
            {
                if (!CurveNameSet.Contains(N))
                {
                    CurveNames.Add(N);
                    CurveNameSet.Add(N);
                }
            }
        }
    }

    CurveValues.SetNumZeroed(CurveNames.Num());
    bCurveCacheInitialized = true;
}

void UO3DSBroadcastComponent::CaptureCurves(USkeletalMeshComponent* SkelComp)
{
    EnsureCurveCache(SkelComp);
    if (!SkelComp)
    {
        return;
    }

    const bool bDebugCurves = (CVarO3DSBroadcastDebugCurves.GetValueOnAnyThread() != 0);

    // Initialize to 0.0
    for (int32 i = 0; i < CurveNames.Num(); ++i)
    {
        CurveValues[i] = 0.0f;
    }
    // Read named curves from the FINAL anim instance: prefer PostProcess instance if present
    UAnimInstance* SourceAnim = nullptr;
    if (USkeletalMeshComponent* SMC = SkelComp)
    {
        // UE 5.x: Post-process anim instance runs after the main graph
        SourceAnim = SMC->GetPostProcessInstance();
        if (!SourceAnim)
        {
            SourceAnim = SMC->GetAnimInstance();
        }
    }

    if (SourceAnim)
    {
        for (int32 i = 0; i < CurveNames.Num(); ++i)
        {
            const FName& Name = CurveNames[i];
            const float V = SourceAnim->GetCurveValue(Name);
            CurveValues[i] = V;
        }
    }

    if (bDebugCurves)
    {
        const int32 LogCount = FMath::Min(5, CurveNames.Num());
        for (int32 i = 0; i < LogCount; ++i)
        {
            UE_LOG(LogO3DSBroadcast, Verbose, TEXT("  Curve[%d] %s = %.4f"), i, *CurveNames[i].ToString(), CurveValues[i]);
        }
    }
}

void UO3DSBroadcastComponent::HandleBoneTransformsFinalized()
{
    if (!bIsCapturing)
    {
        return;
    }

    USkeletalMeshComponent* SkelComp = TargetMesh.Get();
    if (!SkelComp)
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

    // Capture curves after pose
    CaptureCurves(SkelComp);
}

void UO3DSBroadcastComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    // Capture is delegate-driven via HandleBoneTransformsFinalized
}
