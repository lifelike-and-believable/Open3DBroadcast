// Acceptance matrix for src/o3ds/clock_offset.h, per the roadmap doc's A2.b
// spec (docs/roadmap/resilient-streaming-and-motion-prediction.md, §3/A2.b).
#include "test_framework.h"

#include "o3ds/clock_offset.h"

#include <cstdlib>
#include <random>

using namespace O3DS;

namespace
{
	// Deterministic uniform draw in [0, 1), matching the convention already
	// established in channel_model.cpp (avoids std::uniform_real_distribution,
	// whose exact output is unspecified across standard library
	// implementations).
	double NextUniform01(std::mt19937_64& rng)
	{
		const double kTwoPow64 = 18446744073709551616.0;
		return (double)rng() / kTwoPow64;
	}
}

O3DS_TEST(ClockOffset_ConstantSkewNoJitter_ConvergesExactly)
{
	// tx_wallclock advances 16ms/frame (60fps-ish); local_recv = tx_wallclock
	// + a fixed skew of 500ms and zero network jitter (delay_i == 0 for all
	// frames). The rolling-min offset should converge to exactly that skew.
	ClockOffsetEstimator est;
	const int64_t kSkewUs = 500000;
	uint64_t tx = 1000000000ull;

	ClockOffsetEstimator::Sample last;
	for (int i = 0; i < 200; i++)
	{
		uint64_t local_recv = (uint64_t)((int64_t)tx + kSkewUs);
		last = est.Observe(tx, local_recv);
		tx += 16000;
	}

	O3DS_CHECK_EQ(last.offset_estimate_us, kSkewUs);
	O3DS_CHECK_EQ(last.excess_delay_us, (int64_t)0);
}

O3DS_TEST(ClockOffset_SkewPlusBoundedJitter_ConvergesToSkewPlusMinDelay)
{
	// offset_i = skew + delay_i, delay_i in [0, 20ms). The rolling minimum
	// over a window long enough to see many samples should land at (or very
	// near) skew + 0, since delay's infimum is 0 and enough samples are drawn
	// to get arbitrarily close to it.
	ClockOffsetEstimator::Config cfg;
	cfg.window_s = 2.0;
	ClockOffsetEstimator est(cfg);

	const int64_t kSkewUs = -250000; // sender's clock ahead of the receiver's
	const int64_t kMaxDelayUs = 20000;
	uint64_t tx = 5000000000ull;
	std::mt19937_64 rng(12345);

	ClockOffsetEstimator::Sample last;
	int64_t min_offset_seen = INT64_MAX;
	for (int i = 0; i < 2000; i++)
	{
		int64_t delay = (int64_t)(NextUniform01(rng) * kMaxDelayUs);
		int64_t offset = kSkewUs + delay;
		min_offset_seen = std::min(min_offset_seen, offset);
		uint64_t local_recv = (uint64_t)((int64_t)tx + offset);
		last = est.Observe(tx, local_recv);
		tx += 5000; // 5ms/frame, dense enough to fill the 2s window generously
	}

	// The estimator's target (rolling min) can never be below the true
	// per-window minimum offset actually drawn, and by now (many samples,
	// slew long since caught up) offset_estimate_us should sit close to it.
	O3DS_CHECK(last.offset_estimate_us >= min_offset_seen - 1);
	O3DS_CHECK(last.offset_estimate_us <= kSkewUs + 2000); // within 2ms of the true floor
	O3DS_CHECK(last.excess_delay_us >= 0);
}

O3DS_TEST(ClockOffset_MappedPresentationTime_IsMonotonicUnderJitter)
{
	ClockOffsetEstimator::Config cfg;
	cfg.window_s = 1.0;
	cfg.max_slew_rate = 0.3;
	ClockOffsetEstimator est(cfg);

	uint64_t tx = 9000000000ull;
	std::mt19937_64 rng(777);
	uint64_t prevMapped = 0;
	bool first = true;

	for (int i = 0; i < 5000; i++)
	{
		int64_t delay = (int64_t)(NextUniform01(rng) * 30000); // up to 30ms jitter
		uint64_t local_recv = tx + (uint64_t)delay;
		auto s = est.Observe(tx, local_recv);

		if (!first)
			O3DS_CHECK(s.mapped_presentation_time_us >= prevMapped);
		prevMapped = s.mapped_presentation_time_us;
		first = false;

		tx += 4000; // 4ms/frame
	}
}

O3DS_TEST(ClockOffset_ClockStep_IsSlewedNotJumped)
{
	// Simulate a discrete local-clock step: after a long stretch of steady
	// zero-jitter samples, local_recv suddenly jumps forward by 300ms for one
	// step (as if NTP stepped the local clock), then continues steady at the
	// new level. The *reported* estimate must not jump by anywhere near the
	// full step in a single Observe() call - it should move gradually.
	ClockOffsetEstimator::Config cfg;
	cfg.window_s = 5.0;
	cfg.max_slew_rate = 0.1; // slow, deliberately, so the step is easy to observe mid-slew
	ClockOffsetEstimator est(cfg);

	uint64_t tx = 20000000000ull;
	const int64_t kSkewUs = 100000;

	ClockOffsetEstimator::Sample beforeStep;
	for (int i = 0; i < 50; i++)
	{
		beforeStep = est.Observe(tx, (uint64_t)((int64_t)tx + kSkewUs));
		tx += 16000;
	}
	O3DS_CHECK_EQ(beforeStep.offset_estimate_us, kSkewUs);

	// Step: local clock jumps forward 300ms for every subsequent sample.
	const int64_t kStepUs = 300000;
	tx += 16000;
	auto justAfterStep = est.Observe(tx, (uint64_t)((int64_t)tx + kSkewUs + kStepUs));

	// A single 16ms tick at max_slew_rate=0.1 bounds movement to ~1.6us -
	// nowhere close to the 300ms step. Allow generous headroom (10ms) while
	// still proving it did not jump to the new level immediately.
	int64_t movedBy = justAfterStep.offset_estimate_us - beforeStep.offset_estimate_us;
	O3DS_CHECK(movedBy >= 0);
	O3DS_CHECK(movedBy < 10000);

	// Given enough subsequent samples at the new steady level, the estimate
	// must eventually converge there.
	ClockOffsetEstimator::Sample last = justAfterStep;
	for (int i = 0; i < 5000; i++)
	{
		tx += 16000;
		last = est.Observe(tx, (uint64_t)((int64_t)tx + kSkewUs + kStepUs));
	}
	O3DS_CHECK_EQ(last.offset_estimate_us, kSkewUs + kStepUs);
}

O3DS_TEST(ClockOffset_LegacyZeroTimestamp_PassesThroughLocalRecvAndLeavesStateUntouched)
{
	ClockOffsetEstimator est;

	// Establish real state first.
	uint64_t tx = 42000000000ull;
	auto warm = est.Observe(tx, tx + 1000);
	O3DS_CHECK(warm.offset_estimate_us != 0);

	// A legacy/unset frame must not poison the estimator: it should pass
	// through local_recv_us untouched...
	auto legacy = est.Observe(0, 999999999999ull);
	O3DS_CHECK_EQ(legacy.mapped_presentation_time_us, (uint64_t)999999999999ull);
	O3DS_CHECK_EQ(legacy.offset_estimate_us, (int64_t)0);
	O3DS_CHECK_EQ(legacy.excess_delay_us, (int64_t)0);

	// ...and the next real frame should behave exactly as if the legacy
	// frame had never been observed (same offset as before, no discontinuity
	// from a phantom huge sample).
	tx += 16000;
	auto after = est.Observe(tx, tx + 1000);
	O3DS_CHECK_EQ(after.offset_estimate_us, warm.offset_estimate_us);
}
