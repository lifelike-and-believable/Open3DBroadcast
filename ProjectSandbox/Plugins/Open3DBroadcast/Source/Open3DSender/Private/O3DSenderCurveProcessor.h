// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"
#include "O3DSenderCurveConfig.h"

class USkeletalMeshComponent;

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
    // Test-only white-box access (Public/Testing/O3DSenderTesting.h). Unconditional on purpose.
    friend class FO3DSenderCurveProcessorProbe;

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
