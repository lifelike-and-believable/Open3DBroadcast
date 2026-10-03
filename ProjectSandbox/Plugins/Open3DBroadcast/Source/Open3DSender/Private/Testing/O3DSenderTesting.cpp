// Copyright (c) Open3DStream Contributors

#include "Testing/O3DSenderTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderCurveProcessor.h"

FO3DSenderCurveProcessorProbe::FO3DSenderCurveProcessorProbe()
	: Filter(MakeUnique<FO3DSenderCurveFilter>())
{
}

FO3DSenderCurveProcessorProbe::~FO3DSenderCurveProcessorProbe() = default;

void FO3DSenderCurveProcessorProbe::SetCurves(const TArray<FName>& Names, const TArray<float>& Values)
{
	// A new list, as a curve cache refresh makes; the filter resets its last-sent state for it.
	// No morph curves, as before (the probe never marked any).
	TSharedRef<FO3DSCurveList> List = MakeShared<FO3DSCurveList>();
	List->Names = Names;
	List->MorphMask.Init(false, Names.Num());
	CurveList = List;
	CurveValues = Values;
}

void FO3DSenderCurveProcessorProbe::SetCurveValues(const TArray<float>& Values)
{
	CurveValues = Values;
}

void FO3DSenderCurveProcessorProbe::BuildFilteredCurves(const FO3DSenderCurveConfig& Config, TArray<FName>& OutNames, TArray<float>& OutValues)
{
	Filter->Apply(Config, CurveList, CurveValues, OutNames, OutValues);
}

void FO3DSenderCurveProcessorProbe::FilterFrame(FO3DSPoseFrame& Frame)
{
	Filter->FilterFrame(Frame);
}

#endif // WITH_DEV_AUTOMATION_TESTS
