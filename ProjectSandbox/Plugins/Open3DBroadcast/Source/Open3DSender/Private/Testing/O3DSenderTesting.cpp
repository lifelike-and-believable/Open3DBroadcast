// Copyright Lifelike & Believable. All Rights Reserved.

#include "Testing/O3DSenderTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "O3DSenderCurveProcessor.h"

FO3DSenderCurveProcessorProbe::FO3DSenderCurveProcessorProbe()
	: Processor(MakeUnique<FO3DSenderCurveProcessor>())
{
}

FO3DSenderCurveProcessorProbe::~FO3DSenderCurveProcessorProbe() = default;

void FO3DSenderCurveProcessorProbe::SetCurves(const TArray<FName>& Names, const TArray<float>& Values)
{
	FO3DSenderCurveProcessor& P = *Processor;
	P.CurveNames = Names;
	P.CurveValues = Values;
	P.LastSentCurveValues.SetNumZeroed(Names.Num());
	P.LastSentHasValue.SetNumZeroed(Names.Num());
	P.CurveNameSet.Reset();
	for (const FName& Name : Names)
	{
		P.CurveNameSet.Add(Name);
	}
	P.bCurveCacheInitialized = true;
	++P.CurveRevision;
}

void FO3DSenderCurveProcessorProbe::SetCurveValues(const TArray<float>& Values)
{
	Processor->CurveValues = Values;
}

void FO3DSenderCurveProcessorProbe::BuildFilteredCurves(const FO3DSenderCurveConfig& Config, TArray<FName>& OutNames, TArray<float>& OutValues)
{
	Processor->BuildFilteredCurves(Config, OutNames, OutValues);
}

#endif // WITH_DEV_AUTOMATION_TESTS
