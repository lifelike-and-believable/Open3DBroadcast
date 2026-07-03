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
}

#endif
