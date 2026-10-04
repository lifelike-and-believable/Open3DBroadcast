// Copyright Lifelike & Believable. All Rights Reserved.

#include "O3DSceneTimeMapper.h"

#include "Misc/FrameTime.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/wire_format.h"
THIRD_PARTY_INCLUDES_END

namespace O3DSceneTimeMapperPrivate
{
	/** A derived value that would not advance moves forward by this much (seconds): well below a frame at any rate. */
	constexpr double MinStepSeconds = 1.0e-6;
}

TOptional<FQualifiedFrameTime> FO3DSceneTimeMapper::Map(FName Subject, const O3DS::SceneTime* SenderTime, double WorldTime, double NowSeconds,
	const TOptional<FQualifiedFrameTime>& EngineTime)
{
	FSubjectState& State = Subjects.FindOrAdd(Subject);

	double Seconds = 0.0;
	FFrameRate SourceRate;
	bool bFromSender = false;
	if (SenderTime != nullptr && O3DS::IsValidSceneTime(*SenderTime))
	{
		SourceRate = FFrameRate(SenderTime->rate_numerator, SenderTime->rate_denominator);
		Seconds = FQualifiedFrameTime(FFrameTime(FFrameNumber(SenderTime->frame), SenderTime->subframe), SourceRate).AsSeconds();
		State.bHasSenderAnchor = true;
		State.AnchorSceneSeconds = Seconds;
		State.AnchorWorldTime = WorldTime;
		bFromSender = true;
	}
	else if (State.bHasSenderAnchor && State.Rate.IsSet())
	{
		// Continue the sender's timeline rather than jump to this engine's timecode.
		SourceRate = State.Rate.GetValue();
		Seconds = State.AnchorSceneSeconds + (WorldTime - State.AnchorWorldTime);
	}
	else if (EngineTime.IsSet() && EngineTime->Rate.IsValid())
	{
		SourceRate = EngineTime->Rate;
		const double OffsetNow = EngineTime->AsSeconds() - NowSeconds;
		if (!bHasEngineOffset || FMath::Abs(OffsetNow - EngineOffsetSeconds) > SourceRate.AsInterval())
		{
			EngineOffsetSeconds = OffsetNow;
			bHasEngineOffset = true;
		}
		Seconds = WorldTime + EngineOffsetSeconds;
	}
	else
	{
		return TOptional<FQualifiedFrameTime>();
	}

	if (!State.Rate.IsSet())
	{
		State.Rate = SourceRate;
	}
	const FFrameRate Rate = State.Rate.GetValue();

	if (!bFromSender)
	{
		if (State.bHasLast && Seconds <= State.LastSeconds)
		{
			Seconds = State.LastSeconds + O3DSceneTimeMapperPrivate::MinStepSeconds;
		}
		if (Seconds < 0.0)
		{
			return TOptional<FQualifiedFrameTime>(); // LiveLink rejects a negative frame number
		}
	}
	State.bHasLast = true;
	State.LastSeconds = Seconds;
	return FQualifiedFrameTime(Rate.AsFrameTime(Seconds), Rate);
}

void FO3DSceneTimeMapper::Reset()
{
	Subjects.Empty();
	bHasEngineOffset = false;
	EngineOffsetSeconds = 0.0;
}
