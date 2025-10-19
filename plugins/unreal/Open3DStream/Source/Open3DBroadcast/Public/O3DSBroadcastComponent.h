// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "O3DSBroadcastComponent.generated.h"

class USkeletalMeshComponent;
class USkeleton;
class USkeletalMesh;

UCLASS(ClassGroup=(Open3DStream), meta=(BlueprintSpawnableComponent))
class OPEN3DBROADCAST_API UO3DSBroadcastComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UO3DSBroadcastComponent();

    // Start/Stop capture controls (C++)
    UFUNCTION(BlueprintCallable, Category="Open3DStream|Broadcast")
    void StartCapture();

    UFUNCTION(BlueprintCallable, Category="Open3DStream|Broadcast")
    void StopCapture();

    UFUNCTION(BlueprintPure, Category="Open3DStream|Broadcast")
    bool IsCapturing() const { return bIsCapturing; }

    // Target skeletal mesh component to capture; if not set, auto-discovers one on owner at BeginPlay
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast")
    TWeakObjectPtr<USkeletalMeshComponent> TargetMesh;

    // Optional capture rate limit (Hz). <= 0 means capture every evaluation.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Open3DStream|Broadcast")
    float CaptureRateHz = 60.0f;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    void BindToTarget();
    void UnbindFromTarget();
    void HandleBoneTransformsFinalized(USkinnedMeshComponent* SkinnedMesh, bool bRequiredBonesOnly);

    void EnsureSkeletonCache(USkeletalMeshComponent* SkelComp);
    void RefreshSkeletonCache(USkeletalMeshComponent* SkelComp);
    FString BuildSubjectName(const USkeletalMeshComponent* SkelComp) const;
    // Sanitize a subject name to policy: ASCII only, replace whitespace with '_', allow [-._A-Za-z0-9/]
    FString SanitizeSubjectName(const FString& Raw) const;

    // Curves (Morph + Named Anim Curves)
    void EnsureCurveCache(USkeletalMeshComponent* SkelComp);
    void RefreshCurveCache(USkeletalMeshComponent* SkelComp);
    void CaptureCurves(USkeletalMeshComponent* SkelComp);

    // Cache
    TArray<FName> BoneNames;
    TArray<int32> ParentIndices;
    TWeakObjectPtr<USkeleton> CachedSkeleton;
    TWeakObjectPtr<USkeletalMesh> CachedSkeletalMesh;

    // Curve cache and working buffers
    TArray<FName> CurveNames;          // Stable order: Morphs then Anim Curves
    TArray<float> CurveValues;         // Same length as CurveNames
    TSet<FName> MorphNameSet;          // For quick lookup when filling values
    TSet<FName> CurveNameSet;          // To avoid duplicates across sources
    bool bCurveCacheInitialized = false;

    bool bIsCapturing = false;
    double LastCaptureTime = 0.0;
    uint64 FrameCounter = 0;

    // Reserved for future delegate-based capture when available
    FDelegateHandle BoneTransformsFinalizedHandle;
};
