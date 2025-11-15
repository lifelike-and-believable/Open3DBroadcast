#include "O3DSenderCurveProcessor.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "O3DHelpers.h"
#include "O3DSenderLogs.h"

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
        RefreshCurveCache(SkelComp, Config);
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

        float OutValue = 0.0f;
        if (SkelComp->GetCurveValue(Name, 0.0f, OutValue))
        {
            Value = OutValue;
        }
        else if (MorphNameSet.Contains(Name))
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
            if (Config.bLogFilteredCurves)
            {
                UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Dropped curve %s (NaN/Inf)"), *NameString);
            }
            continue;
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

            if (FMath::Abs(Value) < Config.CurveEpsilon)
            {
                if (Config.bLogFilteredCurves)
                {
                    UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (epsilon %.6f) V=%.6f"), *NameString, Config.CurveEpsilon, Value);
                }
                continue;
            }

            const bool bHasLast = LastSentHasValue.IsValidIndex(Index) ? (LastSentHasValue[Index] != 0) : false;
            if (bHasLast)
            {
                const float Last = LastSentCurveValues[Index];
                if (FMath::Abs(Value - Last) < Config.CurveDeltaThreshold)
                {
                    if (Config.bLogFilteredCurves)
                    {
                        UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (delta %.6f < %.6f) V=%.6f Last=%.6f"), *NameString, FMath::Abs(Value - Last), Config.CurveDeltaThreshold, Value, Last);
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

void FO3DSenderCurveProcessor::RefreshCurveCache(USkeletalMeshComponent* SkelComp, const FO3DSenderCurveConfig& Config)
{
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

    if (ShouldFilterByPatterns(Config.IncludeCurvePatterns) || ShouldFilterByPatterns(Config.ExcludeCurvePatterns))
    {
        TArray<FName> Filtered;
        Filtered.Reserve(CurveNames.Num());
        for (const FName& Name : CurveNames)
        {
            if (IsCurveAllowedByPatterns(Name.ToString(), Config))
            {
                Filtered.Add(Name);
            }
        }
        CurveNames = MoveTemp(Filtered);
        CurveNameSet.Reset();
        for (const FName& Name : CurveNames)
        {
            CurveNameSet.Add(Name);
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

bool FO3DSenderCurveProcessor::IsCurveAllowedByPatterns(const FString& Name, const FO3DSenderCurveConfig& Config) const
{
    return EvaluatePatternForName(Name, Config);
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