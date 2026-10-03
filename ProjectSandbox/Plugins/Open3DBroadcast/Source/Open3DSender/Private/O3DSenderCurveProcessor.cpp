// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSenderCurveProcessor.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimInstance.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "O3DHelpers.h"
#include "O3DSenderComponent.h"
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

// ── Capture (game thread) ───────────────────────────────────────────────────────────────

void FO3DSenderCurveProcessor::Reset()
{
    CurveNames.Reset();
    MorphNameSet.Reset();
    CurveNameSet.Reset();
    CurveList.Reset();
    bCurveCacheInitialized = false;
}

void FO3DSenderCurveProcessor::InvalidateCache()
{
    bCurveCacheInitialized = false;
}

void FO3DSenderCurveProcessor::EnsureCurveCache(USkeletalMeshComponent* SkelComp)
{
    if (!bCurveCacheInitialized)
    {
        RefreshCurveCache(SkelComp);
    }
}

void FO3DSenderCurveProcessor::CaptureCurves(USkeletalMeshComponent* SkelComp, bool bDebugCurves, TArray<float>& OutValues) const
{
    OutValues.Reset();
    if (!SkelComp)
    {
        return;
    }

    OutValues.SetNumZeroed(CurveNames.Num());

    const TMap<FName, float>& MorphOverrides = SkelComp->GetMorphTargetCurves();

    for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
    {
        const FName& Name = CurveNames[Index];
        float Value = 0.0f;

        if (MorphNameSet.Contains(Name))
        {
            if (const float* Override = MorphOverrides.Find(Name))
            {
                OutValues[Index] = *Override;
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

        OutValues[Index] = Value;

        if (bDebugCurves && Index < 5)
        {
            UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Curve[%d] %s = %.4f"), Index, *Name.ToString(), Value);
        }
    }
}

void FO3DSenderCurveProcessor::RefreshCurveCache(USkeletalMeshComponent* SkelComp)
{
    // Every curve on the mesh and skeleton is cached. Include/exclude patterns are applied per frame
    // by FO3DSenderCurveFilter, only while bEnableCurveFiltering is on, so turning filtering off or
    // editing the patterns at runtime takes effect on the next frame (SND-20).
    CurveNames.Reset();
    MorphNameSet.Reset();
    CurveNameSet.Reset();

    if (SkelComp)
    {
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
    }

    // A new shared list on every refresh, so the filter sees a new pointer and resets its
    // last-sent state, as the refresh used to do in place.
    TSharedRef<FO3DSCurveList> NewList = MakeShared<FO3DSCurveList>();
    NewList->Names = CurveNames;
    NewList->MorphMask.Init(false, CurveNames.Num());
    for (int32 Index = 0; Index < CurveNames.Num(); ++Index)
    {
        if (MorphNameSet.Contains(CurveNames[Index]))
        {
            NewList->MorphMask[Index] = true;
        }
    }
    CurveList = NewList;

    bCurveCacheInitialized = true;
}

// ── Filtering (after sampling; no UObject access) ───────────────────────────────────────

void FO3DSenderCurveFilter::Reset()
{
    LastList.Reset();
    LastSentCurveValues.Reset();
    LastSentHasValue.Reset();
    PatternCache.Reset();
}

void FO3DSenderCurveFilter::FilterFrame(FO3DSPoseFrame& Frame)
{
    if (!Frame.CurveList.IsValid())
    {
        return;
    }

    const FO3DSenderEncodingSettings& Settings = Frame.Encoding;
    FO3DSenderCurveConfig Config;
    Config.bClampMorphCurvesToUnit = Settings.bClampMorphCurvesToUnit;
    Config.bDropNaNAndInfinity = Settings.bDropNaNAndInfinity;
    Config.bEnableCurveFiltering = Settings.bEnableCurveFiltering;
    Config.bApplyValueFilters = Settings.bApplyCurveValueFilters;
    Config.CurveEpsilon = Settings.CurveEpsilon;
    Config.CurveDeltaThreshold = Settings.CurveDeltaThreshold;
    // Point into the frame's shared, immutable pattern lists, which outlive this call.
    Config.IncludeCurvePatterns = Settings.IncludeCurvePatterns.Get();
    Config.ExcludeCurvePatterns = Settings.ExcludeCurvePatterns.Get();
    Config.bLogFilteredCurves = Settings.bLogFilteredCurves;

    Apply(Config, Frame.CurveList, Frame.RawCurveValues, Frame.CurveNames, Frame.CurveValues);
}

void FO3DSenderCurveFilter::OnCurveListChanged(const TSharedPtr<const FO3DSCurveList>& List)
{
    LastList = List;
    const int32 Num = List.IsValid() ? List->Names.Num() : 0;
    LastSentCurveValues.Reset();
    LastSentCurveValues.SetNumZeroed(Num);
    LastSentHasValue.Reset();
    LastSentHasValue.SetNumZeroed(Num);
    PatternCache.Reset();
}

void FO3DSenderCurveFilter::Apply(const FO3DSenderCurveConfig& Config, const TSharedPtr<const FO3DSCurveList>& List, const TArray<float>& RawValues,
    TArray<FName>& OutNames, TArray<float>& OutValues)
{
    OutNames.Reset();
    OutValues.Reset();
    if (!List.IsValid())
    {
        return;
    }

    if (List != LastList)
    {
        OnCurveListChanged(List);
    }

    const TArray<FName>& Names = List->Names;
    OutNames.Reserve(Names.Num());
    OutValues.Reserve(Names.Num());

    const bool bFilteringEnabled = Config.bEnableCurveFiltering;
    if (bFilteringEnabled)
    {
        UpdatePatternCacheIfNeeded(Config, *List);
    }

    for (int32 Index = 0; Index < Names.Num(); ++Index)
    {
        const FName& Name = Names[Index];
        // The name as text is needed only for verbose logs and uncached pattern matching; building
        // it for every curve on every frame was a large part of this filter's cost.
        float Value = RawValues.IsValidIndex(Index) ? RawValues[Index] : 0.0f;

        if (Config.bDropNaNAndInfinity && !FMath::IsFinite(Value))
        {
            // Treated as 0, as the property documents. Dropping the curve instead would change the
            // curve list for this frame, which the residual and quantized encodings answer with a
            // full sync (SND-3).
            if (Config.bLogFilteredCurves)
            {
                UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Curve %s is NaN/Inf; sending 0"), *Name.ToString());
            }
            Value = 0.0f;
        }

        const bool bIsMorph = List->MorphMask.IsValidIndex(Index) && List->MorphMask[Index];
        if (Config.bClampMorphCurvesToUnit && bIsMorph)
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
                bPatternAllowed = EvaluatePatternForName(Name.ToString(), Config);
            }

            if (!bPatternAllowed)
            {
                if (Config.bLogFilteredCurves)
                {
                    UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (pattern)"), *Name.ToString());
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
                        UE_LOG(LogO3DSenderComponent, Verbose, TEXT("Filtered curve %s (epsilon %.6f, delta %.6f) V=%.6f Last=%.6f"), *Name.ToString(), Config.CurveEpsilon, Config.CurveDeltaThreshold, Value, Last);
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

void FO3DSenderCurveFilter::UpdatePatternCacheIfNeeded(const FO3DSenderCurveConfig& Config, const FO3DSCurveList& List)
{
    if (!Config.bEnableCurveFiltering)
    {
        PatternCache.Reset();
        return;
    }

    // The cache is reset whenever the curve list changes (OnCurveListChanged).
    const uint32 DesiredHash = ComputePatternHash(Config);
    const bool bNeedsRebuild = !PatternCache.bValid
        || (PatternCache.PatternHash != DesiredHash)
        || (PatternCache.AllowedMask.Num() != List.Names.Num());

    if (!bNeedsRebuild)
    {
        return;
    }

    PatternCache.PatternHash = DesiredHash;
    PatternCache.bValid = true;
    PatternCache.AllowedMask.Init(true, List.Names.Num());
    PatternCache.bHasActiveFilters = ShouldFilterByPatterns(Config.IncludeCurvePatterns) || ShouldFilterByPatterns(Config.ExcludeCurvePatterns);

    if (!PatternCache.bHasActiveFilters)
    {
        return;
    }

    for (int32 Index = 0; Index < List.Names.Num(); ++Index)
    {
        const bool bAllowed = EvaluatePatternForName(List.Names[Index].ToString(), Config);
        PatternCache.AllowedMask[Index] = bAllowed;
    }
}

uint32 FO3DSenderCurveFilter::ComputePatternHash(const FO3DSenderCurveConfig& Config)
{
    uint32 Hash = Config.bEnableCurveFiltering ? 0x1u : 0u;
    Hash = HashCombineFast(Hash, HashPatternList(Config.IncludeCurvePatterns));
    Hash = HashCombineFast(Hash, HashPatternList(Config.ExcludeCurvePatterns));
    return Hash;
}

uint32 FO3DSenderCurveFilter::HashPatternList(const TArray<FString>* Patterns)
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

bool FO3DSenderCurveFilter::EvaluatePatternForName(const FString& Name, const FO3DSenderCurveConfig& Config)
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
