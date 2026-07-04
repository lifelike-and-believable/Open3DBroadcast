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

#include "reorder_gate.h"

namespace O3DS
{
	ReorderGate::ReorderGate()
		: ReorderGate(Config())
	{
	}

	ReorderGate::ReorderGate(const Config& config)
		: mConfig(config)
	{
	}

	void ReorderGate::Push(Frame&& frame, double now_s, const std::function<void(Frame&&)>& emit)
	{
		// Legacy/unset: no ordering info at all. Pass straight through
		// exactly like pre-A1 behavior (this preserves old-sender interop,
		// and lets a single stream mix sequenced and unsequenced frames
		// without the unsequenced ones disturbing gate state for the rest).
		if (frame.seq == 0)
		{
			emit(std::move(frame));
			return;
		}

		// Restart detection. frame_epoch, when present, must be treated as an
		// ORDERED value, not just "changed vs not": a network can reorder a
		// straggler from an OLDER session to arrive after the first frame of
		// a new one (this is exactly the kind of reordering this whole gate
		// exists to handle), and naively treating any epoch != mLastEpoch as
		// a restart would misapply that stale frame as if it were current -
		// and, if it also let mLastEpoch regress, would make every
		// subsequent legitimate frame look like yet another restart.
		bool isRestart = false;
		if (frame.epoch != 0)
		{
			if (mHaveEpoch && frame.epoch < mLastEpoch)
			{
				// Straggler from an older session, not a restart. Drop it
				// like any other out-of-window frame; do NOT touch mLastEpoch.
				mStats.stale_dropped++;
				CheckTimeouts(now_s, emit);
				return;
			}

			if (mHaveEpoch && frame.epoch > mLastEpoch)
			{
				isRestart = true;
			}

			// Only ever advances - a legacy (epoch==0) frame or an already-
			// rejected older-epoch straggler must never move this backward.
			if (frame.epoch > mLastEpoch)
			{
				mLastEpoch = frame.epoch;
			}
			mHaveEpoch = true;
		}
		else if (mInitialized && !mHaveEpoch && frame.seq <= mLastApplied
			&& (mLastApplied - frame.seq) > mConfig.reset_backjump)
		{
			// Legacy-only fallback (frame_epoch unavailable on this stream):
			// only consulted when we have no better signal, since a stream
			// that does carry epochs already resolves restarts above.
			isRestart = true;
		}

		if (isRestart)
		{
			mPending.clear();
			mRecentlyDelivered.clear();
			mInitialized = false;
		}

		if (!mInitialized)
		{
			mInitialized = true;
			mLastApplied = frame.seq - 1;
		}
		else if (frame.seq <= mLastApplied)
		{
			if (mRecentlyDelivered.count(frame.seq))
				mStats.dup_dropped++;
			else
				mStats.stale_dropped++;

			CheckTimeouts(now_s, emit);
			return;
		}

		if (frame.seq == mLastApplied + 1)
		{
			DeliverAndDrain(std::move(frame), emit, /*countDrainAsReordered*/ true);
		}
		else
		{
			// Gap: buffer this frame and remember when we started waiting.
			// A resend of a seq we're already holding (duplicate key) is a
			// genuine duplicate arrival - the original buffered copy (and
			// its wait timer) is kept, but still count it.
			uint64_t seq = frame.seq;
			auto result = mPending.emplace(seq, PendingEntry{ std::move(frame), now_s });
			if (!result.second)
				mStats.dup_dropped++;
		}

		CheckTimeouts(now_s, emit);
	}

	void ReorderGate::Flush(double now_s, const std::function<void(Frame&&)>& emit)
	{
		CheckTimeouts(now_s, emit);
	}

	void ReorderGate::DeliverAndDrain(Frame&& frame, const std::function<void(Frame&&)>& emit, bool countDrainAsReordered)
	{
		uint64_t seq = frame.seq;
		emit(std::move(frame));
		mStats.delivered++;
		mLastApplied = seq;
		mRecentlyDelivered.insert(seq);
		PruneRecentlyDelivered();

		// Drain any buffered consecutive successors this delivery unblocked.
		for (;;)
		{
			auto it = mPending.find(mLastApplied + 1);
			if (it == mPending.end())
				break;

			Frame next = std::move(it->second.frame);
			mPending.erase(it);

			uint64_t nextSeq = next.seq;
			emit(std::move(next));
			mStats.delivered++;
			if (countDrainAsReordered)
				mStats.reordered++; // arrived out of order relative to its predecessor; caught up now
			mLastApplied = nextSeq;
			mRecentlyDelivered.insert(nextSeq);
			PruneRecentlyDelivered();
		}
	}

	void ReorderGate::CheckTimeouts(double now_s, const std::function<void(Frame&&)>& emit)
	{
		// Give up waiting on the lowest pending gap once it's been buffered
		// too long or the buffer has grown past max_window; everything
		// strictly below its seq that we never received counts as lost.
		// Loop (not just once) since giving up on one gap can immediately
		// expose the next one if multiple holes exist at once.
		while (!mPending.empty())
		{
			auto first = mPending.begin();
			bool windowFull = mPending.size() >= mConfig.max_window;
			bool timedOut = (now_s - first->second.buffered_at_s) >= mConfig.max_delay_s;

			if (!windowFull && !timedOut)
				break; // still within budget; keep waiting for the hole to fill

			uint64_t gapSeq = first->first;
			uint64_t gapCount = gapSeq - (mLastApplied + 1);
			mStats.lost += gapCount;

			Frame frame = std::move(first->second.frame);
			mPending.erase(first);

			mLastApplied = gapSeq - 1; // pretend caught up to just before the frame we're giving up on
			// These frames were simply waiting behind a hole we're giving up
			// on, not reordered relative to each other - don't inflate the
			// reordered stat for frames that arrived in perfectly good order.
			DeliverAndDrain(std::move(frame), emit, /*countDrainAsReordered*/ false);
		}
	}

	void ReorderGate::PruneRecentlyDelivered()
	{
		// Bound memory to roughly the same window as gap buffering. A
		// duplicate arriving so late it falls outside this window gets
		// classified as stale_dropped instead of dup_dropped - a diagnostic
		// difference only; either way the frame is correctly dropped.
		if (mLastApplied < mConfig.max_window)
			return;

		uint64_t cutoff = mLastApplied - mConfig.max_window;
		auto it = mRecentlyDelivered.begin();
		while (it != mRecentlyDelivered.end() && *it < cutoff)
			it = mRecentlyDelivered.erase(it);
	}
}
