// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Maps an audio source clock (the mixer's AudioClock, a capture device's StreamTimeSec) onto the
 * sender clock, FPlatformTime::Seconds() (ADR 0008 item 7, ADR 0009 item 7, SND-17).
 *
 * The first buffer sets Offset = Now - Source; each stamp is Source + Offset. Later buffers move
 * the offset toward the measured one with a low-pass filter whose time constant is in seconds of
 * source time, so device drift does not accumulate and delivery jitter barely moves the stamps.
 * A discontinuity of more than JumpThresholdSec resets the offset to the measured one:
 * - at once when the source clock goes backwards or gets ahead of the sender clock (a delivery
 *   delay can only make the measured offset larger, never smaller);
 * - when the measured offset stays above the estimate for PersistSec of sender time, so one
 *   delayed callback (a hitch) and the burst that follows it do not move the stamps.
 *
 * The stamp is when the buffer reached the producer, mapped back through the source clock: it
 * carries the mean delivery delay (about one buffer plus driver latency) as a constant bias.
 *
 * Not thread-safe: one mapper per producer, used by one thread at a time.
 */
class FO3DAudioClockMapper
{
public:
	static constexpr double JumpThresholdSec = 0.1;
	static constexpr double FilterTimeConstantSec = 5.0;
	static constexpr double PersistSec = 0.5;

	/** The sender-clock time of a buffer whose source clock reads SourceSec, seen at NowSec. */
	double Map(double SourceSec, double NowSec)
	{
		const double Measured = NowSec - SourceSec;
		if (!bHasOffset)
		{
			ResetTo(Measured);
		}
		else
		{
			const double SourceStep = SourceSec - LastSourceSec;
			const double Deviation = Measured - Offset;
			if (SourceStep < 0.0 || Deviation < -JumpThresholdSec)
			{
				ResetTo(Measured);
				++ResetCount;
			}
			else if (Deviation > JumpThresholdSec)
			{
				if (AboveSinceSec < 0.0)
				{
					AboveSinceSec = NowSec;
				}
				else if (NowSec - AboveSinceSec >= PersistSec)
				{
					ResetTo(Measured);
					++ResetCount;
				}
			}
			else
			{
				AboveSinceSec = -1.0;
				const double Step = FMath::Clamp(SourceStep, 0.0, FilterTimeConstantSec);
				const double Alpha = 1.0 - FMath::Exp(-Step / FilterTimeConstantSec);
				Offset += Alpha * Deviation;
			}
		}
		LastSourceSec = SourceSec;
		return SourceSec + Offset;
	}

	/** Forget the offset; the next buffer sets it again. Call when a stream (re)starts. */
	void Reset()
	{
		bHasOffset = false;
		Offset = 0.0;
		LastSourceSec = 0.0;
		AboveSinceSec = -1.0;
	}

	double GetOffset() const { return Offset; }
	/** Discontinuities seen since construction (not cleared by Reset). */
	int32 GetResetCount() const { return ResetCount; }

private:
	void ResetTo(double Measured)
	{
		bHasOffset = true;
		Offset = Measured;
		AboveSinceSec = -1.0;
	}

	bool bHasOffset = false;
	double Offset = 0.0;
	double LastSourceSec = 0.0;
	/** Sender time at which the measured offset first rose above the jump threshold; -1 if not above. */
	double AboveSinceSec = -1.0;
	int32 ResetCount = 0;
};
