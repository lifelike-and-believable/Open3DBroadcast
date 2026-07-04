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

#ifndef OPEN3D_STREAM_CHANNEL_MODEL_H
#define OPEN3D_STREAM_CHANNEL_MODEL_H

#include "reorder_gate.h" // reuses O3DS::Frame - this is the B2/A1 interlock

#include <cstdint>
#include <functional>
#include <map>
#include <random>
#include <utility>

namespace O3DS
{
	struct ChannelConfig
	{
		uint64_t seed = 0;
		double loss_prob = 0.0;      // [0,1]
		double base_latency_s = 0.0; // fixed delay added to every delivered frame
		double max_jitter_s = 0.0;   // additional delay, drawn uniformly from [0, max_jitter_s)
		double dup_prob = 0.0;       // [0,1]
	};

	//! Ground-truth counters describing what the channel actually did, so a
	//! test can assert a downstream ReorderGate's stats against them
	//! directly (this is the whole point of B2 as a test harness for A1).
	struct ChannelStats
	{
		uint64_t dropped = 0;
		uint64_t duplicated = 0;
		uint64_t passed = 0;    // frames actually released (excludes drops, includes both copies of a dup)
		uint64_t reordered = 0; // frames released with a lower seq than one already released (seq==0 excluded - no ordering semantics, matches ReorderGate's own legacy bypass)
	};

	//! A deterministic network-condition simulator: sits between a frame
	//! source (e.g. Workstream B's replay driver) and a sink (e.g. A1's
	//! ReorderGate, or a real transport) and reproduces loss/latency/
	//! jitter/duplication.
	//!
	//! Frames are released to `emit` in DELIVERY-TIME order, not arrival
	//! order - this is what makes jitter alone produce realistic reordering
	//! without a separate "reorder" knob: a frame that draws a small jitter
	//! can be released before an earlier-pushed frame that drew a larger
	//! one.
	//!
	//! Determinism: seeded explicitly with std::mt19937_64, but draws are
	//! taken directly from the raw generator (see NextUniform01 in the
	//! .cpp), NOT via std::uniform_real_distribution/std::normal_distribution
	//! - the C++ standard does not specify those distributions' algorithms,
	//! so their output can differ between standard library implementations
	//! even for the same engine and seed. That would silently break
	//! "deterministic across runs/platforms," which is the entire reason
	//! this class exists as a test fixture.
	//!
	//! Not thread-safe: confine one instance to a single thread, same as
	//! ReorderGate.
	class ChannelModel
	{
	public:
		explicit ChannelModel(const ChannelConfig& config);

		//! Offers one frame to the channel. `emit_t` is the time (any
		//! consistent clock/units - seconds since an arbitrary reference is
		//! typical) this frame was made available to the channel; it is
		//! used only to compute this frame's delivery time and to release
		//! any earlier-scheduled frames whose delivery time has now passed
		//! (a frame's own delivery time never depends on frames pushed
		//! after it). May call `emit` zero or more times per call.
		//!
		//! Draws are made in a fixed order every call - loss, then jitter,
		//! then duplication - regardless of outcome, so the RNG sequence a
		//! given seed produces depends only on the seed and the number of
		//! frames pushed, not on which ones happened to be dropped.
		void Push(Frame&& frame, double emit_t, const std::function<void(Frame&&)>& emit);

		//! Releases everything still pending, in delivery-time order,
		//! regardless of whether its scheduled delivery time has actually
		//! "arrived" - call once the source has no more frames to offer.
		void Flush(const std::function<void(Frame&&)>& emit);

		const ChannelStats& Stats() const { return mStats; }

	private:
		void ReleaseDue(double now, const std::function<void(Frame&&)>& emit);
		void ReleaseOne(Frame&& frame, const std::function<void(Frame&&)>& emit);
		void Enqueue(Frame&& frame, double deliverAt);

		ChannelConfig mConfig;
		ChannelStats mStats;
		std::mt19937_64 mRng;

		// Keyed by (deliver_at, insertion_index): primary sort by scheduled
		// delivery time, insertion_index as an explicit, deterministic
		// tiebreak (rather than relying on std::multimap's unspecified-by-
		// the-reader equal-key ordering) for frames scheduled at the exact
		// same delivery time (this happens routinely for duplicates, which
		// are scheduled together - see Push()).
		std::map<std::pair<double, uint64_t>, Frame> mPending;
		uint64_t mNextInsertionIndex = 0;

		bool mHaveReleasedSequenced = false;
		uint64_t mMaxSeqReleased = 0;
	};
}

#endif
