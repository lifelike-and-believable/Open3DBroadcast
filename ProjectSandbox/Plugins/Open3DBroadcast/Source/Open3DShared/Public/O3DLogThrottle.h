// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

#include <atomic>

/**
 * Lets one caller log per interval, from any thread, and counts the calls it turned away, for
 * log lines on hot paths: per frame, per datagram, or per message from a peer (WP-R3, TR-6;
 * AGENTS.md: hot paths stay quiet in logs). One instance per log site, usually a member.
 *
 *     int64 Suppressed = 0;
 *     if (OversizeLog.ShouldLog(Suppressed))
 *     {
 *         UE_LOG(LogX, Warning, TEXT("Datagram too large (%d bytes); %lld similar since the last warning."), Bytes, Suppressed);
 *     }
 */
class FO3DLogThrottle
{
public:
	explicit FO3DLogThrottle(double InIntervalSeconds = 2.0)
		: IntervalSeconds(InIntervalSeconds)
	{
	}

	FO3DLogThrottle(const FO3DLogThrottle&) = delete;
	FO3DLogThrottle& operator=(const FO3DLogThrottle&) = delete;

	/**
	 * True for the one call that may log now; OutSuppressed is then the number of calls turned
	 * away since the last true. NowSeconds is for tests.
	 */
	bool ShouldLog(int64& OutSuppressed, double NowSeconds = FPlatformTime::Seconds())
	{
		double Previous = LastLogSeconds.load(std::memory_order_relaxed);
		if (NowSeconds - Previous >= IntervalSeconds && LastLogSeconds.compare_exchange_strong(Previous, NowSeconds))
		{
			OutSuppressed = Suppressed.exchange(0, std::memory_order_relaxed);
			return true;
		}
		Suppressed.fetch_add(1, std::memory_order_relaxed);
		OutSuppressed = 0;
		return false;
	}

	bool ShouldLog()
	{
		int64 Ignored = 0;
		return ShouldLog(Ignored);
	}

private:
	const double IntervalSeconds;
	std::atomic<double> LastLogSeconds{ -1.0e300 };
	std::atomic<int64> Suppressed{ 0 };
};
