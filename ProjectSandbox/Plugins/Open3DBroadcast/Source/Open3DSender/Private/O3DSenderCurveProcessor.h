#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"

class USkeletalMeshComponent;

/** Tunable curve filtering parameters shared between the sender component and processor. */
struct FO3DSenderCurveConfig
{
    bool bClampMorphCurvesToUnit = true;
    bool bDropNaNAndInfinity = true;
    /** Master switch for include/exclude patterns and the value filters below (SND-20). */
    bool bEnableCurveFiltering = false;
    /**
     * Apply the per-frame epsilon/delta value filters. Only meaningful with bEnableCurveFiltering;
     * the component turns it off for the residual and quantized encodings (ADR 0005 (ii), SND-3).
     */
    bool bApplyValueFilters = false;
    float CurveEpsilon = 0.0005f;
    float CurveDeltaThreshold = 0.001f;
    const TArray<FString>* IncludeCurvePatterns = nullptr;
    const TArray<FString>* ExcludeCurvePatterns = nullptr;
    bool bLogFilteredCurves = false;
};

/**
 * Helper that caches the available animation curves on a skeletal mesh component, collects per-frame
 * values, and applies threshold/filter rules before they are forwarded to the serializer.
 */
class FO3DSenderCurveProcessor
{
public:
    void Reset();
    void InvalidateCache();

    void EnsureCurveCache(USkeletalMeshComponent* SkelComp, const FO3DSenderCurveConfig& Config);
    void CaptureCurves(USkeletalMeshComponent* SkelComp, bool bDebugCurves);
    void BuildFilteredCurves(const FO3DSenderCurveConfig& Config, TArray<FName>& OutNames, TArray<float>& OutValues);

    const TArray<FName>& GetCurveNames() const { return CurveNames; }
    const TArray<float>& GetCurveValues() const { return CurveValues; }

private:
    friend struct FO3DSenderWireTestAccess; // test-only white-box access (see O3DSenderComponent.h)

    void RefreshCurveCache(USkeletalMeshComponent* SkelComp);
    void UpdatePatternCacheIfNeeded(const FO3DSenderCurveConfig& Config);
    uint32 ComputePatternHash(const FO3DSenderCurveConfig& Config) const;
    static uint32 HashPatternList(const TArray<FString>* Patterns);
    bool EvaluatePatternForName(const FString& Name, const FO3DSenderCurveConfig& Config) const;

private:
    TArray<FName> CurveNames;
    TArray<float> CurveValues;
    TArray<float> LastSentCurveValues;
    TArray<uint8> LastSentHasValue;
    TSet<FName> MorphNameSet;
    TSet<FName> CurveNameSet;
    bool bCurveCacheInitialized = false;
    int32 CurveRevision = 0;

    struct FCurvePatternCache
    {
        uint32 PatternHash = 0;
        int32 CachedCurveRevision = -1;
        TBitArray<> AllowedMask;
        bool bHasActiveFilters = false;

        void Reset()
        {
            PatternHash = 0;
            CachedCurveRevision = -1;
            AllowedMask.Reset();
            bHasActiveFilters = false;
        }
    };

    FCurvePatternCache PatternCache;
};
