// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"
#include "O3DSenderCurveConfig.h"

class USkeletalMeshComponent;
struct FO3DSCurveList;
struct FO3DSPoseFrame;

/**
 * Curve capture, on the game thread: caches the animation curves a skeletal mesh component exposes
 * and samples their raw values each frame. Filtering is not done here: FO3DSenderCurveFilter applies
 * it to the sampled frame afterwards (WP-A2a, ADR 0008 item 1).
 */
class FO3DSenderCurveProcessor
{
public:
    void Reset();
    void InvalidateCache();

    void EnsureCurveCache(USkeletalMeshComponent* SkelComp);

    /**
     * Samples one raw value per cached curve into OutValues (resized to the curve count; capacity
     * kept), in the order of GetCurveList(). No clamping, NaN handling or filtering.
     */
    void CaptureCurves(USkeletalMeshComponent* SkelComp, bool bDebugCurves, TArray<float>& OutValues) const;

    /** The cached curve list, shared with every frame sampled against it. Null before the cache is built. */
    const TSharedPtr<const FO3DSCurveList>& GetCurveList() const { return CurveList; }

private:
    void RefreshCurveCache(USkeletalMeshComponent* SkelComp);

private:
    TArray<FName> CurveNames;
    TSet<FName> MorphNameSet;
    TSet<FName> CurveNameSet;
    TSharedPtr<const FO3DSCurveList> CurveList;
    bool bCurveCacheInitialized = false;
};

/**
 * Curve filtering after sampling (WP-A2a, ADR 0008 item 1): clamps morph curves, replaces NaN/Inf,
 * applies the include/exclude patterns and the epsilon/delta value filters (SND-4, SND-20). It works
 * only from a sampled frame and its settings snapshot and never touches a UObject, so it can run on
 * the WP-A2c worker. It keeps the last value sent per curve; a new curve list resets that state.
 */
class FO3DSenderCurveFilter
{
public:
    /** Forgets the last-sent values and the pattern cache (StartCapture/StopCapture). */
    void Reset();

    /**
     * Fills Frame.CurveNames/CurveValues from Frame.CurveList and Frame.RawCurveValues with the
     * frame's own settings (Frame.Encoding). A frame without a CurveList is left unchanged.
     */
    void FilterFrame(FO3DSPoseFrame& Frame);

    /** The filter itself. RawValues has one entry per List.Names entry (a missing one counts as 0). */
    void Apply(const FO3DSenderCurveConfig& Config, const TSharedPtr<const FO3DSCurveList>& List, const TArray<float>& RawValues,
        TArray<FName>& OutNames, TArray<float>& OutValues);

private:
    void OnCurveListChanged(const TSharedPtr<const FO3DSCurveList>& List);
    void UpdatePatternCacheIfNeeded(const FO3DSenderCurveConfig& Config, const FO3DSCurveList& List);
    static uint32 ComputePatternHash(const FO3DSenderCurveConfig& Config);
    static uint32 HashPatternList(const TArray<FString>* Patterns);
    static bool EvaluatePatternForName(const FString& Name, const FO3DSenderCurveConfig& Config);

private:
    /** The list LastSent* refer to; held so a new list is told apart by pointer. */
    TSharedPtr<const FO3DSCurveList> LastList;
    TArray<float> LastSentCurveValues;
    TArray<uint8> LastSentHasValue;

    struct FCurvePatternCache
    {
        uint32 PatternHash = 0;
        bool bValid = false;
        TBitArray<> AllowedMask;
        bool bHasActiveFilters = false;

        void Reset()
        {
            PatternHash = 0;
            bValid = false;
            AllowedMask.Reset();
            bHasActiveFilters = false;
        }
    };

    FCurvePatternCache PatternCache;
};
