#pragma once

#include "CoreMinimal.h"

class USkeletalMeshComponent;

struct FO3DSenderCurveConfig
{
    bool bClampMorphCurvesToUnit = true;
    bool bDropNaNAndInfinity = true;
    bool bEnableCurveFiltering = false;
    float CurveEpsilon = 0.0005f;
    float CurveDeltaThreshold = 0.001f;
    const TArray<FString>* IncludeCurvePatterns = nullptr;
    const TArray<FString>* ExcludeCurvePatterns = nullptr;
    bool bLogFilteredCurves = false;
};

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
    void RefreshCurveCache(USkeletalMeshComponent* SkelComp, const FO3DSenderCurveConfig& Config);
    bool IsCurveAllowedByPatterns(const FString& Name, const FO3DSenderCurveConfig& Config) const;

private:
    TArray<FName> CurveNames;
    TArray<float> CurveValues;
    TArray<float> LastSentCurveValues;
    TArray<uint8> LastSentHasValue;
    TSet<FName> MorphNameSet;
    TSet<FName> CurveNameSet;
    bool bCurveCacheInitialized = false;
};