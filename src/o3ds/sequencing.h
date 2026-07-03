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

#ifndef OPEN3D_STREAM_SEQUENCING_H
#define OPEN3D_STREAM_SEQUENCING_H

#include <atomic>
#include <cstdint>

namespace O3DS
{
	//! Monotonic sequence generator for a single logical sender->receiver
	//! stream (see SubjectList's tx_seq field). Deliberately per-stream, not
	//! a process-global counter: sharing one counter across fan-out to
	//! multiple receivers would make each receiver's view have "gaps" that
	//! are just other receivers' frames, not real loss - one instance should
	//! be owned by whatever object represents a single outbound stream
	//! (e.g. the per-transport serializer), not shared across streams.
	class SequenceCounter
	{
	public:
		SequenceCounter() : mNext(1) {}

		//! Returns the next sequence number, starting at 1. 0 is reserved to
		//! mean "unset" on the wire (see SubjectList.tx_seq), so this never
		//! returns 0.
		uint64_t Next()
		{
			return mNext.fetch_add(1, std::memory_order_relaxed);
		}

		//! Reset back to the initial state (new session/epoch on this stream).
		void Reset()
		{
			mNext.store(1, std::memory_order_relaxed);
		}

	private:
		std::atomic<uint64_t> mNext;
	};

	//! Current UTC time in microseconds since the Unix epoch, for
	//! SubjectList.tx_wallclock_us. This is a wall clock (not monotonic) and
	//! can jump under NTP adjustment - it is transmitted for latency/
	//! staleness estimation only. Frame *ordering* must never depend on it;
	//! use tx_seq for that.
	uint64_t NowUtcMicros();

	//! Generates a value for SubjectList.frame_epoch. Call this ONCE per
	//! publisher session (e.g. alongside SequenceCounter::Reset(), when a
	//! stream (re)starts) - never per frame.
	//!
	//! ReorderGate (reorder_gate.h) relies on frame_epoch being strictly
	//! greater on every new session than on the previous one, so it can
	//! tell a restart from an old-session straggler that simply arrived
	//! late. This returns wall-clock seconds since the Unix epoch
	//! (truncated to 32 bits, wrapping only once every ~136 years - not a
	//! practical concern), which is monotonic across restarts in virtually
	//! every real deployment and is never 0 in any deployable timeframe.
	//!
	//! Known limitation: two restarts of the same stream within the same
	//! wall-clock second produce the same epoch, which ReorderGate would
	//! then be unable to distinguish from a continuing session - a restart
	//! that fast falls back to being detected only via the (weaker)
	//! backward-jump heuristic, or not at all if the new session's
	//! sequence happens to climb back above the old one before the gate
	//! would otherwise flag it as stale. A future revision could use a
	//! persisted counter instead if this proves insufficient in practice.
	uint32_t NewSessionEpoch();
}

#endif
