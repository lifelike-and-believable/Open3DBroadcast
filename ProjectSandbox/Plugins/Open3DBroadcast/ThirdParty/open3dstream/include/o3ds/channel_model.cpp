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

#include "channel_model.h"

#include <cstdint>

namespace
{
	// 2^64, exactly representable as a double (unlike 2^64-1, which would
	// round up to 2^64 anyway - using the exact power-of-two literal makes
	// that explicit rather than relying on rounding to coincidentally land
	// on the right value).
	const double kTwoPow64 = 18446744073709551616.0;

	// Manual uniform [0,1) draw from the raw generator - deliberately NOT
	// std::uniform_real_distribution (see channel_model.h's class comment
	// for why). mt19937_64's own output range [0, 2^64-1] is exactly and
	// portably specified by the standard, so this division is reproducible
	// across any conforming implementation.
	double NextUniform01(std::mt19937_64& rng)
	{
		return (double)rng() / kTwoPow64;
	}
}

namespace O3DS
{
	ChannelModel::ChannelModel(const ChannelConfig& config)
		: mConfig(config)
		, mRng(config.seed)
	{
	}

	void ChannelModel::Push(Frame&& frame, double emit_t, const std::function<void(Frame&&)>& emit)
	{
		ReleaseDue(emit_t, emit);

		// Fixed per-frame draw order (loss, jitter, dup) regardless of
		// outcome, so the RNG sequence depends only on the seed and the
		// number of frames pushed so far - not on which ones were dropped.
		double lossDraw = NextUniform01(mRng);
		double jitterDraw = NextUniform01(mRng);
		double dupDraw = NextUniform01(mRng);

		bool dropped = lossDraw < mConfig.loss_prob;
		if (dropped)
		{
			mStats.dropped++;
			return; // a dropped frame is never delivered and never duplicated
		}

		double jitter = jitterDraw * mConfig.max_jitter_s;
		double deliverAt = emit_t + mConfig.base_latency_s + jitter;

		bool duplicate = dupDraw < mConfig.dup_prob;
		if (duplicate)
		{
			// Both copies scheduled at the identical delivery time - a
			// duplicate is modeled as arriving together with the original,
			// not as an independently-delayed second delivery.
			Frame copy = frame;
			Enqueue(std::move(frame), deliverAt);
			Enqueue(std::move(copy), deliverAt);
			mStats.duplicated++;
		}
		else
		{
			Enqueue(std::move(frame), deliverAt);
		}
	}

	void ChannelModel::Flush(const std::function<void(Frame&&)>& emit)
	{
		while (!mPending.empty())
		{
			auto it = mPending.begin();
			Frame frame = std::move(it->second);
			mPending.erase(it);
			ReleaseOne(std::move(frame), emit);
		}
	}

	void ChannelModel::ReleaseDue(double now, const std::function<void(Frame&&)>& emit)
	{
		while (!mPending.empty())
		{
			auto it = mPending.begin(); // lowest (deliver_at, insertion_index)
			if (it->first.first > now)
				break;

			Frame frame = std::move(it->second);
			mPending.erase(it);
			ReleaseOne(std::move(frame), emit);
		}
	}

	void ChannelModel::ReleaseOne(Frame&& frame, const std::function<void(Frame&&)>& emit)
	{
		// seq == 0 (legacy/unset) has no ordering semantics, matching
		// ReorderGate's own bypass - excluded here so a run of legacy
		// frames doesn't get spuriously marked "reordered" against
		// whatever the last real sequence number happened to be.
		if (frame.seq != 0)
		{
			if (mHaveReleasedSequenced && frame.seq < mMaxSeqReleased)
				mStats.reordered++;
			if (!mHaveReleasedSequenced || frame.seq > mMaxSeqReleased)
				mMaxSeqReleased = frame.seq;
			mHaveReleasedSequenced = true;
		}

		mStats.passed++;
		emit(std::move(frame));
	}

	void ChannelModel::Enqueue(Frame&& frame, double deliverAt)
	{
		uint64_t index = mNextInsertionIndex++;
		mPending.emplace(std::make_pair(deliverAt, index), std::move(frame));
	}
}
