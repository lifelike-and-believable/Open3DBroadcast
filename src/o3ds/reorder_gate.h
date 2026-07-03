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

#ifndef OPEN3D_STREAM_REORDER_GATE_H
#define OPEN3D_STREAM_REORDER_GATE_H

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace O3DS
{
	//! One frame's worth of wire bytes plus the sequencing metadata a
	//! ReorderGate needs to make ordering decisions. The gate never inspects
	//! `bytes` - it is opaque payload, handed to `emit` unparsed so a frame
	//! that gets buffered and later dropped (superseded, stale, etc.) is
	//! never paid for with a full O3DS::SubjectList::Parse().
	struct Frame
	{
		uint64_t seq = 0;          // SubjectList.tx_seq from the wire (0 = legacy/unset)
		uint64_t wallclock_us = 0; // SubjectList.tx_wallclock_us; pass-through only - the
		                           // gate's own gap-timeout bookkeeping uses the caller-supplied
		                           // `now_s` clock (see Push/Flush below), never this field
		uint32_t epoch = 0;        // SubjectList.frame_epoch (0 = legacy/unset)
		std::vector<char> bytes;   // raw wire bytes, unparsed
	};

	struct ReorderStats
	{
		uint64_t delivered = 0;
		uint64_t dup_dropped = 0;
		uint64_t stale_dropped = 0;
		uint64_t lost = 0;
		uint64_t reordered = 0;
	};

	//! Per-source (per-sender-stream) reordering/dedup/stale-drop gate.
	//! Not thread-safe: confine one instance to a single thread (see A2.a).
	//!
	//! Frames are delivered to `emit` in ascending tx_seq order. A gap
	//! (missing seq) buffers later frames for up to Config::max_window
	//! entries or Config::max_delay_s, whichever comes first, then gives up
	//! and delivers what it has, counting the hole as `lost`.
	//!
	//! Publisher restarts (the sender's SequenceCounter resets to 1) are
	//! detected via frame_epoch when the stream carries it: an epoch strictly
	//! greater than the last one seen means a new session. frame_epoch is
	//! treated as an ORDERED value, not just "changed vs not" - a smaller
	//! epoch than the last one seen is a straggler from an older session
	//! reordered in flight across the restart boundary (a real network
	//! condition, not a hypothetical), and is dropped as stale rather than
	//! mistaken for yet another restart; it never moves the tracked epoch
	//! backward. Senders should generate frame_epoch with
	//! O3DS::NewSessionEpoch() (sequencing.h), which is best-effort
	//! monotonic across restarts (derived from wall-clock seconds) - see
	//! its own doc comment for the failure mode this doesn't cover.
	//! Config::reset_backjump is used only as a fallback for streams that
	//! never set frame_epoch (legacy senders): once a stream has shown a
	//! non-zero epoch, the backjump heuristic is not consulted again for it.
	class ReorderGate
	{
	public:
		struct Config
		{
			uint32_t max_window = 16;       // max frames buffered waiting on one gap
			double max_delay_s = 0.05;      // max time to wait on one gap, in the caller's `now_s` clock
			uint64_t reset_backjump = 256;  // legacy-only (no frame_epoch) restart heuristic threshold
		};

		ReorderGate();
		explicit ReorderGate(const Config& config);

		//! Feed one incoming frame. May synchronously call `emit` zero or
		//! more times (zero if buffered pending a gap or dropped as
		//! dup/stale; one or more if it fills a gap and drains successors).
		//! `now_s` is the caller's own clock (any monotonic seconds-based
		//! clock is fine; it is only ever compared to other `now_s` values
		//! passed to this same gate instance) - NOT derived from the
		//! frame's wallclock_us.
		void Push(Frame&& frame, double now_s, const std::function<void(Frame&&)>& emit);

		//! Check pending gaps for timeout even when no new frame has
		//! arrived; call periodically (e.g. once per receiver tick) so a
		//! stalled gap doesn't wait forever for a Push that never comes.
		void Flush(double now_s, const std::function<void(Frame&&)>& emit);

		const ReorderStats& Stats() const { return mStats; }

	private:
		struct PendingEntry
		{
			Frame frame;
			double buffered_at_s;
		};

		// countDrainAsReordered distinguishes two callers: when a frame
		// fills a genuine gap (its predecessor arrived, and this frame had
		// been waiting because it arrived ahead of that predecessor), any
		// successors drained after it really were reordered relative to
		// what unblocked them - true. When CheckTimeouts gives up on a lost
		// gap instead, the frames it then drains were simply waiting behind
		// a hole, in perfectly good order relative to each other - false,
		// or they would inflate the reordered stat for frames that were
		// never actually reordered.
		void DeliverAndDrain(Frame&& frame, const std::function<void(Frame&&)>& emit, bool countDrainAsReordered);
		void CheckTimeouts(double now_s, const std::function<void(Frame&&)>& emit);
		void PruneRecentlyDelivered();

		Config mConfig;
		ReorderStats mStats;

		bool mInitialized = false;
		uint64_t mLastApplied = 0;

		bool mHaveEpoch = false;
		uint32_t mLastEpoch = 0;

		std::map<uint64_t, PendingEntry> mPending;    // seq -> buffered frame, ordered by seq
		std::set<uint64_t> mRecentlyDelivered;        // bounded window, for dup-vs-stale classification
	};
}

#endif
