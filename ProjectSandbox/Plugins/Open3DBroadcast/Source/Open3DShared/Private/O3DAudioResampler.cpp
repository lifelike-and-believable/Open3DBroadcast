// Copyright (c) Open3DStream Contributors

#include "O3DAudioResampler.h"

namespace
{
	int64 GreatestCommonDivisor(int64 A, int64 B)
	{
		while (B != 0)
		{
			const int64 T = A % B;
			A = B;
			B = T;
		}
		return A;
	}
}

FO3DAudioResampler::FO3DAudioResampler() = default;
FO3DAudioResampler::~FO3DAudioResampler() = default;

bool FO3DAudioResampler::Configure(int32 InSampleRate, int32 OutSampleRate, int32 InNumChannels)
{
	if (InSampleRate <= 0 || OutSampleRate <= 0 || InNumChannels <= 0)
	{
		InRate = 0;
		OutRate = 0;
		NumChannels = 0;
		return false;
	}

	if (InSampleRate == InRate && OutSampleRate == OutRate && InNumChannels == NumChannels)
	{
		return true;
	}

	InRate = InSampleRate;
	OutRate = OutSampleRate;
	NumChannels = InNumChannels;

	const int64 Divisor = GreatestCommonDivisor(InRate, OutRate);
	PositionDenominator = OutRate / Divisor;
	PositionStep = InRate / Divisor;

	Sections.Reset();
	if (OutRate < InRate)
	{
		// 8th-order Butterworth low-pass as four biquads (RBJ cookbook coefficients). The Q of
		// section k is 1 / (2 sin((2k - 1) * pi / 16)).
		static const double SectionQ[4] = { 2.56291545, 0.89997622, 0.60134489, 0.50979558 };
		const double CutoffHz = 0.45 * static_cast<double>(OutRate);
		const double W0 = 2.0 * UE_DOUBLE_PI * CutoffHz / static_cast<double>(InRate);
		const double CosW0 = FMath::Cos(W0);
		const double SinW0 = FMath::Sin(W0);
		for (const double Q : SectionQ)
		{
			const double Alpha = SinW0 / (2.0 * Q);
			const double A0 = 1.0 + Alpha;
			FBiquad& Section = Sections.AddDefaulted_GetRef();
			Section.B0 = static_cast<float>(((1.0 - CosW0) * 0.5) / A0);
			Section.B1 = static_cast<float>((1.0 - CosW0) / A0);
			Section.B2 = Section.B0;
			Section.A1 = static_cast<float>((-2.0 * CosW0) / A0);
			Section.A2 = static_cast<float>((1.0 - Alpha) / A0);
		}
	}

	Reset();
	return true;
}

void FO3DAudioResampler::Reset()
{
	NextPosition = 0;
	bHasPreviousFrame = false;
	PreviousFrame.SetNumZeroed(NumChannels);
	FilterState.SetNumZeroed(Sections.Num() * NumChannels * 2);
}

int32 FO3DAudioResampler::Process(const float* InInterleaved, int32 NumFrames, TArray<float>& OutInterleaved, double& OutFirstFrameOffsetSec)
{
	OutInterleaved.Reset();
	OutFirstFrameOffsetSec = 0.0;
	if (!IsConfigured() || !InInterleaved || NumFrames <= 0)
	{
		return 0;
	}

	const float* Source = InInterleaved;
	if (Sections.Num() > 0)
	{
		Filtered.SetNumUninitialized(NumFrames * NumChannels, EAllowShrinking::No);
		const int32 NumSections = Sections.Num();
		for (int32 Frame = 0; Frame < NumFrames; ++Frame)
		{
			for (int32 Channel = 0; Channel < NumChannels; ++Channel)
			{
				float Sample = InInterleaved[Frame * NumChannels + Channel];
				for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
				{
					const FBiquad& S = Sections[SectionIndex];
					float* Z = &FilterState[(SectionIndex * NumChannels + Channel) * 2];
					const float Y = S.B0 * Sample + Z[0];
					Z[0] = S.B1 * Sample - S.A1 * Y + Z[1];
					Z[1] = S.B2 * Sample - S.A2 * Y;
					Sample = Y;
				}
				Filtered[Frame * NumChannels + Channel] = Sample;
			}
		}
		Source = Filtered.GetData();
	}

	if (!bHasPreviousFrame)
	{
		FMemory::Memcpy(PreviousFrame.GetData(), Source, sizeof(float) * static_cast<SIZE_T>(NumChannels));
		bHasPreviousFrame = true;
	}

	OutFirstFrameOffsetSec = static_cast<double>(NextPosition) / static_cast<double>(PositionDenominator) / static_cast<double>(InRate);

	// Output frames whose position is before the last input frame; a position exactly on the
	// last frame is produced by the next call (as offset -1, from PreviousFrame).
	const int64 Limit = static_cast<int64>(NumFrames - 1) * PositionDenominator;
	const int64 Estimate = (Limit - NextPosition) / PositionStep + 2;
	OutInterleaved.Reserve(static_cast<int32>(FMath::Max<int64>(Estimate, 0)) * NumChannels);
	while (NextPosition < Limit)
	{
		const int64 Index0 = NextPosition >= 0 ? NextPosition / PositionDenominator : -1;
		const float Fraction = static_cast<float>(static_cast<double>(NextPosition - Index0 * PositionDenominator) / static_cast<double>(PositionDenominator));
		const float* Sample0 = Index0 < 0 ? PreviousFrame.GetData() : Source + Index0 * NumChannels;
		const float* Sample1 = Source + (Index0 + 1) * NumChannels;
		for (int32 Channel = 0; Channel < NumChannels; ++Channel)
		{
			OutInterleaved.Add(Sample0[Channel] + (Sample1[Channel] - Sample0[Channel]) * Fraction);
		}
		NextPosition += PositionStep;
	}
	NextPosition -= static_cast<int64>(NumFrames) * PositionDenominator;

	FMemory::Memcpy(PreviousFrame.GetData(), Source + static_cast<int64>(NumFrames - 1) * NumChannels, sizeof(float) * static_cast<SIZE_T>(NumChannels));
	return OutInterleaved.Num() / NumChannels;
}
