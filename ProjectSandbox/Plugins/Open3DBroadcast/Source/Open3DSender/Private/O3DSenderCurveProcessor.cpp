#include "O3DSenderCurveProcessor.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimInstance.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "O3DHelpers.h"
#include "O3DSenderLogs.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/sender_sync.h"
THIRD_PARTY_INCLUDES_END

namespace
{
    bool ShouldFilterByPatterns(const TArray<FString>* Patterns)
    {
        return Patterns && Patterns->Num() > 0;
    }

    bool MatchesPattern(const FString& Text, const FString& Pattern)
    {
        return !Pattern.IsEmpty() && O3DHelpers::NameMatchesPattern(Text, Pattern);
    }
}

void FO3DSenderCurveProcessor::Reset()
{
    CurveNames.Reset();
    CurveValues.Reset();
    LastSentCurveValues.Reset();
    LastSentHasValue.Reset();
    MorphNameSet.Reset();
    CurveNameSet.Reset();
    bCurveCacheInitialized = false;
    CurveRevision = 0;
    PatternCache.Reset();
}

void FO3DSenderCurveProcessor::InvalidateCache()
{
    bCurveCacheInitialized = false;
    PatternCache.Reset();
}

void FO3DSenderCurveProcessor::EnsureCurveCache(USkeletalMeshComponent* SkelComp, const FO3DSenderCurveConfig& Config)
{
    if (!bCurveCacheInitialized)
    {
        RefreshCurveCache(SkelComp);
    }
}

void FO3DSenderCurveProcessor::CaptureCurves(USkeletalMeshComponent* SkelComp, bool bDebugCurves)
{
    if (!SkelComp)
    {
        return;
    }

    const TMap<FName, float>& MorphOverrides = SkelComp->GetMorphTargetCurves();

    for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
    {
        CurveValues[Index] = 0.0f;
    }

    for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
    {
        const FName& Name = CurveNames[Index];
        float Value = 0.0f;

        if (MorphNameSet.Contains(Name))
        {
            if (const float* Override = MorphOverrides.Find(Name))
            {
                CurveValues[Index] = *Override;
                continue;
            }
        }

        // In UE 5.4+, curve values are accessed from AnimInstance, not GetCurveValue
        // Try to get from AnimInstance first
        if (UAnimInstance* AnimInstance = SkelComp->GetAnimInstance())
        {
            // GetCurveValue on AnimInstance to get evaluated curve
            Value = AnimInstance->GetCurveValue(Name);
        }

        // Fallback for morphs
        if (Value == 0.0f && MorphNameSet.Contains(Name))
        {
            Value = SkelComp->GetMorphTarget(Name);
        }

        CurveValues[Index] = Value;

        if (bDebugCurves && Index < 5)
        {
            UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Curve[%d] %s = %.4f"), Index, *Name.ToString(), Value);
        }
    }
}

void FO3DSenderCurveProcessor::BuildFilteredCurves(const FO3DSenderCurveConfig& Config, TArray<FName>& OutNames, TArray<float>& OutValues)
{
    OutNames.Reset();
    OutValues.Reset();

    OutNames.Reserve(CurveNames.Num());
    OutValues.Reserve(CurveNames.Num());

    const bool bFilteringEnabled = Config.bEnableCurveFiltering;
    if (bFilteringEnabled)
    {
        UpdatePatternCacheIfNeeded(Config);
    }

    for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
    {
        const FName& Name = CurveNames[Index];
        const FString NameString = Name.ToString();
        float Value = CurveValues[Index];

        if (Config.bDropNaNAndInfinity && !FMath::IsFinite(Value))
        {
            // Treated as 0, as the property documents. Dropping the curve instead would change the
            // curve list for this frame, which the residual and quantized encodings answer with a
            // full sync (SND-3).
            if (Config.bLogFilteredCurves)
            {
                UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Curve %s is NaN/Inf; sending 0"), *NameString);
            }
            Value = 0.0f;
        }

        if (Config.bClampMorphCurvesToUnit && MorphNameSet.Contains(Name))
        {
            Value = FMath::Clamp(Value, 0.0f, 1.0f);
        }

        if (bFilteringEnabled)
        {
            bool bPatternAllowed = true;
            if (PatternCache.bHasActiveFilters)
            {
                const bool bMaskValid = PatternCache.AllowedMask.IsValidIndex(Index);
                bPatternAllowed = bMaskValid ? PatternCache.AllowedMask[Index] : true;
            }
            else if (ShouldFilterByPatterns(Config.IncludeCurvePatterns) || ShouldFilterByPatterns(Config.ExcludeCurvePatterns))
            {
                bPatternAllowed = EvaluatePatternForName(NameString, Config);
            }

            if (!bPatternAllowed)
            {
                if (Config.bLogFilteredCurves)
                {
                    UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (pattern)"), *NameString);
                }
                continue;
            }

            if (Config.bApplyValueFilters)
            {
                // SND-4: a curve returning to (near) zero is sent once as exactly 0, then suppressed.
                const bool bHasLast = LastSentHasValue.IsValidIndex(Index) ? (LastSentHasValue[Index] != 0) : false;
                const float Last = (bHasLast && LastSentCurveValues.IsValidIndex(Index)) ? LastSentCurveValues[Index] : 0.0f;
                if (!O3DS::FilterCurveValue(Value, bHasLast, Last, Config.CurveEpsilon, Config.CurveDeltaThreshold))
                {
                    if (Config.bLogFilteredCurves)
                    {
                        UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (epsilon %.6f, delta %.6f) V=%.6f Last=%.6f"), *NameString, Config.CurveEpsilon, Config.CurveDeltaThreshold, Value, Last);
                    }
                    continue;
                }
            }
        }

        OutNames.Add(Name);
        OutValues.Add(Value);

        if (LastSentCurveValues.IsValidIndex(Index))
        {
            LastSentCurveValues[Index] = Value;
            LastSentHasValue[Index] = 1;
        }
    }
}

void FO3DSenderCurveProcessor::RefreshCurveCache(USkeletalMeshComponent* SkelComp)
{
    // Every curve on the mesh and skeleton is cached. Include/exclude patterns are applied per frame
    // in BuildFilteredCurves, only while bEnableCurveFiltering is on, so turning filtering off or
    // editing the patterns at runtime takes effect on the next frame (SND-20).
    CurveNames.Reset();
    CurveValues.Reset();
    LastSentCurveValues.Reset();
    LastSentHasValue.Reset();
    MorphNameSet.Reset();
    CurveNameSet.Reset();

    if (!SkelComp)
    {
        bCurveCacheInitialized = true;
        return;
    }

    USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset();
    USkeleton* Skeleton = SkelMesh ? SkelMesh->GetSkeleton() : nullptr;

    if (SkelMesh)
    {
        const TArray<UMorphTarget*>& Morphs = SkelMesh->GetMorphTargets();
        CurveNames.Reserve(Morphs.Num());
        for (UMorphTarget* Morph : Morphs)
        {
            if (!Morph)
            {
                continue;
            }
            const FName Name = Morph->GetFName();
            if (!CurveNameSet.Contains(Name))
            {
                MorphNameSet.Add(Name);
                CurveNames.Add(Name);
                CurveNameSet.Add(Name);
            }
        }
    }

    if (Skeleton)
    {
        TArray<FName> SkeletonCurveNames;
        Skeleton->GetCurveMetaDataNames(SkeletonCurveNames);
        for (const FName& CurveName : SkeletonCurveNames)
        {
            if (CurveName != NAME_None && !CurveNameSet.Contains(CurveName))
            {
                CurveNames.Add(CurveName);
                CurveNameSet.Add(CurveName);
            }
        }
    }

    CurveNames.Sort([](const FName& A, const FName& B)
    {
        return FCString::Strcmp(*A.ToString(), *B.ToString()) < 0;
    });

    CurveValues.SetNumZeroed(CurveNames.Num());
    LastSentCurveValues.SetNumZeroed(CurveNames.Num());
    LastSentHasValue.SetNumZeroed(CurveNames.Num());

    ++CurveRevision;
    PatternCache.Reset();

    bCurveCacheInitialized = true;
}

void FO3DSenderCurveProcessor::UpdatePatternCacheIfNeeded(const FO3DSenderCurveConfig& Config)
{
    if (!Config.bEnableCurveFiltering)
    {
        PatternCache.Reset();
        return;
    }

    const uint32 DesiredHash = ComputePatternHash(Config);
    const bool bNeedsRebuild = (PatternCache.PatternHash != DesiredHash)
        || (PatternCache.CachedCurveRevision != CurveRevision)
        || (PatternCache.AllowedMask.Num() != CurveNames.Num());

    if (!bNeedsRebuild)
    {
        return;
    }

    PatternCache.PatternHash = DesiredHash;
    PatternCache.CachedCurveRevision = CurveRevision;
    PatternCache.AllowedMask.Init(true, CurveNames.Num());
    PatternCache.bHasActiveFilters = ShouldFilterByPatterns(Config.IncludeCurvePatterns) || ShouldFilterByPatterns(Config.ExcludeCurvePatterns);

    if (!PatternCache.bHasActiveFilters)
    {
        return;
    }

    for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
    {
        const bool bAllowed = EvaluatePatternForName(CurveNames[Index].ToString(), Config);
        PatternCache.AllowedMask[Index] = bAllowed;
    }
}

uint32 FO3DSenderCurveProcessor::ComputePatternHash(const FO3DSenderCurveConfig& Config) const
{
    uint32 Hash = Config.bEnableCurveFiltering ? 0x1u : 0u;
    Hash = HashCombineFast(Hash, HashPatternList(Config.IncludeCurvePatterns));
    Hash = HashCombineFast(Hash, HashPatternList(Config.ExcludeCurvePatterns));
    return Hash;
}

uint32 FO3DSenderCurveProcessor::HashPatternList(const TArray<FString>* Patterns)
{
    if (!Patterns)
    {
        return 0u;
    }

    uint32 Hash = ::GetTypeHash(Patterns->Num());
    for (const FString& Pattern : *Patterns)
    {
        Hash = HashCombineFast(Hash, GetTypeHash(Pattern));
    }
    return Hash;
}

bool FO3DSenderCurveProcessor::EvaluatePatternForName(const FString& Name, const FO3DSenderCurveConfig& Config) const
{
    if (ShouldFilterByPatterns(Config.ExcludeCurvePatterns))
    {
        for (const FString& Pattern : *Config.ExcludeCurvePatterns)
        {
            if (MatchesPattern(Name, Pattern))
            {
                return false;
            }
        }
    }

    if (!ShouldFilterByPatterns(Config.IncludeCurvePatterns))
    {
        return true;
    }

    for (const FString& Pattern : *Config.IncludeCurvePatterns)
    {
        if (MatchesPattern(Name, Pattern))
        {
            return true;
        }
    }

    return false;
}