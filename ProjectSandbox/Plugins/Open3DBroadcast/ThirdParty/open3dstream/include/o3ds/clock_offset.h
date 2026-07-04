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

#ifndef OPEN3D_STREAM_CLOCK_OFFSET_H
#define OPEN3D_STREAM_CLOCK_OFFSET_H

#include <cstdint>
#include <deque>

namespace O3DS
{
	//! Maps a sender's tx_wallclock_us onto the receiver's local clock so a
	//! presentation layer (LiveLink) can be driven consistently across the
	//! network, without requiring the two machines' clocks to be NTP-synced
	//! (see roadmap doc, section 2.5 / A2.b).
	//!
	//! For each observed frame, offset_i = local_recv_i - tx_wallclock_i =
	//! skew + delay_i, where delay_i >= 0. The minimum offset over a bounded
	//! window approximates `skew + min_delay` (the least-delayed frame is
	//! closest to pure clock skew) - the classic NTP-style estimate. This
	//! class tracks that rolling minimum, then *slews* the value it actually
	//! hands back toward the rolling minimum at a bounded rate (rather than
	//! snapping to it) so a discrete local-clock step (NTP adjustment) or a
	//! sudden path-delay change doesn't show up as a presentation-time
	//! discontinuity.
	//!
	//! Precondition: Observe() must be called with samples in non-decreasing
	//! tx_wallclock_us order - i.e. fed from frames already delivered in
	//! ascending tx_seq order (A1's ReorderGate does this before this
	//! estimator ever sees a frame, per A2.a's wiring). This is what makes
	//! the "mapped presentation time is monotonic" guarantee hold: as long as
	//! Config::max_slew_rate < 1.0, the slewed offset can never fall faster
	//! than tx_wallclock_us rises, so tx_wallclock_us + offset_estimate_us
	//! never goes backward. Feeding it out-of-order samples doesn't corrupt
	//! internal state (window pruning is safe either way), but the Sample
	//! returned for that one out-of-order call may not honor monotonicity.
	//!
	//! tx_wallclock_us == 0 (legacy/unset, see SubjectList.tx_wallclock_us)
	//! is treated as "no timestamp available": Observe() returns
	//! local_recv_us as the mapped time and leaves all internal state
	//! untouched, rather than feeding a huge phantom "offset" (now minus the
	//! Unix epoch) into the window.
	class ClockOffsetEstimator
	{
	public:
		struct Config
		{
			double window_s = 5.0;        //!< rolling-min window width, in tx_wallclock_us time
			double max_slew_rate = 0.2;   //!< max fraction of elapsed real time the used estimate
			                               //!< may move toward the rolling-min target per second;
			                               //!< clamped to [0, 1) to preserve the monotonicity guarantee
		};

		struct Sample
		{
			uint64_t mapped_presentation_time_us = 0; //!< tx_wallclock_us + slewed offset estimate
			int64_t  offset_estimate_us = 0;          //!< current slewed offset (~ skew + min_delay)
			int64_t  excess_delay_us = 0;              //!< this frame's offset above the rolling-min
			                                            //!< floor (jitter indicator); always >= 0
		};

		ClockOffsetEstimator();
		explicit ClockOffsetEstimator(const Config& config);

		//! Observe one frame's send/receive timestamps (both UTC microseconds
		//! since the Unix epoch) and return the mapped presentation time plus
		//! diagnostics.
		Sample Observe(uint64_t tx_wallclock_us, uint64_t local_recv_us);

	private:
		struct WindowEntry
		{
			uint64_t tx_wallclock_us;
			int64_t offset_us;
		};

		Config mConfig;
		bool mInitialized = false;
		double mEstimateUs = 0.0; // kept as double for sub-microsecond slew precision
		uint64_t mLastTxWallclockUs = 0;

		std::deque<WindowEntry> mWindow; // monotonic increasing by offset_us; front = window minimum
	};
}

#endif
