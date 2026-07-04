/*
Open 3D Stream

Copyright 2026 Alastair Macleod

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "clock_offset.h"

#include <algorithm>
#include <cmath>

namespace O3DS
{
	namespace
	{
		double ClampSlewRate(double rate)
		{
			if (!(rate > 0.0)) return 0.0;   // also catches NaN
			if (rate >= 1.0) return 0.999999; // strictly < 1 preserves the monotonicity guarantee
			return rate;
		}

		double ClampWindowSeconds(double s)
		{
			return (s > 0.0) ? s : 0.001;
		}
	}

	ClockOffsetEstimator::ClockOffsetEstimator()
		: ClockOffsetEstimator(Config())
	{
	}

	ClockOffsetEstimator::ClockOffsetEstimator(const Config& config)
		: mConfig(config)
	{
		mConfig.window_s = ClampWindowSeconds(mConfig.window_s);
		mConfig.max_slew_rate = ClampSlewRate(mConfig.max_slew_rate);
	}

	ClockOffsetEstimator::Sample ClockOffsetEstimator::Observe(uint64_t tx_wallclock_us, uint64_t local_recv_us)
	{
		Sample result;

		// Legacy/unset timestamp: fall back to local receive time and leave
		// the estimator's state untouched (see class doc comment).
		if (tx_wallclock_us == 0)
		{
			result.mapped_presentation_time_us = local_recv_us;
			result.offset_estimate_us = 0;
			result.excess_delay_us = 0;
			return result;
		}

		const int64_t offset_us = (int64_t)local_recv_us - (int64_t)tx_wallclock_us;

		// Sliding-window minimum: maintain a monotonic-increasing deque of
		// offsets so the front is always the current window's minimum in
		// O(1) amortized per sample.
		while (!mWindow.empty() && mWindow.back().offset_us >= offset_us)
			mWindow.pop_back();
		mWindow.push_back({ tx_wallclock_us, offset_us });

		const int64_t window_us = (int64_t)(mConfig.window_s * 1.0e6);
		// Signed comparison: if tx_wallclock_us ever arrives out of the
		// documented non-decreasing order, this simply skips eviction rather
		// than wrapping around (which unsigned subtraction would do here).
		while (!mWindow.empty() &&
		       (int64_t)tx_wallclock_us - (int64_t)mWindow.front().tx_wallclock_us > window_us)
			mWindow.pop_front();

		const int64_t target_us = mWindow.front().offset_us;

		if (!mInitialized)
		{
			mEstimateUs = (double)target_us;
			mInitialized = true;
		}
		else
		{
			// dt_s bounds how far the estimate may move this call; a
			// non-positive dt (out-of-order tx_wallclock_us) contributes no
			// slew budget rather than a negative one.
			double dt_s = 0.0;
			if (tx_wallclock_us > mLastTxWallclockUs)
				dt_s = (double)(tx_wallclock_us - mLastTxWallclockUs) / 1.0e6;

			const double max_step = mConfig.max_slew_rate * dt_s * 1.0e6;
			double delta = (double)target_us - mEstimateUs;
			delta = std::max(-max_step, std::min(max_step, delta));
			mEstimateUs += delta;
		}
		mLastTxWallclockUs = tx_wallclock_us;

		result.offset_estimate_us = (int64_t)std::llround(mEstimateUs);
		result.excess_delay_us = offset_us - target_us;
		result.mapped_presentation_time_us = (uint64_t)((int64_t)tx_wallclock_us + result.offset_estimate_us);
		return result;
	}
}
