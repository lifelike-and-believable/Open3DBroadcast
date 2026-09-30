#pragma once

#include "CoreMinimal.h"

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
