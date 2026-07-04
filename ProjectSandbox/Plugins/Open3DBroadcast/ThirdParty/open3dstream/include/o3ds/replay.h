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

#ifndef OPEN3D_STREAM_REPLAY_H
#define OPEN3D_STREAM_REPLAY_H

#include "capture.h"
#include "reorder_gate.h" // O3DS::Frame

#include <functional>
#include <istream>

namespace O3DS
{
	enum class ReplayTiming
	{
		AsFastAsPossible, // ignore recorded timing entirely - for deterministic tests
		Realtime,         // sleep between records for the recorded inter-arrival delta
	};

	struct ReplayConfig
	{
		ReplayTiming timing = ReplayTiming::AsFastAsPossible;
		double speed = 1.0;   // Realtime only: inter-arrival waits are divided by this (2.0 = 2x speed)
		bool loop = false;    // rewind and continue after reaching the end of the capture
	};

	//! Sink callback for ReplayCapture: receives each record as a Frame,
	//! plus the frame's original capture time in seconds (record's
	//! recv_wallclock_us / 1e6) - a stable time reference for feeding into
	//! a ChannelModel's `emit_t`, independent of how fast replay actually
	//! executes (this matters for AsFastAsPossible mode and for keeping
	//! tests deterministic regardless of test-runner speed). Return false
	//! to stop replay - the only way to end a `loop=true` replay, and also
	//! useful to bound a test to N frames.
	using ReplaySink = std::function<bool(Frame&& frame, double capture_time_s)>;

	//! Reads a .o3dscap capture from `in` (via the B1 reader) and emits
	//! each record to `sink`. Each record's wire_bytes are interpreted via
	//! SubjectList::PeekMeta to populate the Frame's seq/wallclock_us/epoch;
	//! a record whose wire_bytes aren't a valid SubjectList buffer is
	//! emitted with those left at 0 (legacy/unset), matching how
	//! ReorderGate itself treats untagged frames, rather than being
	//! silently dropped.
	//!
	//! A truncated trailing record (per the B1 reader's own tolerance) is
	//! not an error: everything successfully read is still emitted, then
	//! this returns true, same as reaching a clean end normally.
	//!
	//! Returns false only if the capture's header itself is invalid.
	//! `outHeader`, if non-null, receives the parsed header on success.
	//!
	//! Requires `in` to be seekable if `config.loop` is true (needs to
	//! rewind to the start); a non-seekable stream with loop=false works
	//! for a single pass.
	bool ReplayCapture(std::istream& in, const ReplayConfig& config,
		const ReplaySink& sink, CaptureHeaderInfo* outHeader = nullptr);
}

#endif
