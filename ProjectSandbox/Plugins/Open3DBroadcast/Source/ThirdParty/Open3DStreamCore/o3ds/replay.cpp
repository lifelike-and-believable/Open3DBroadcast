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

#include "replay.h"

#include "model.h"

#include <chrono>
#include <thread>

namespace O3DS
{
	bool ReplayCapture(std::istream& in, const ReplayConfig& config,
		const ReplaySink& sink, CaptureHeaderInfo* outHeader)
	{
		CaptureHeaderInfo header;
		if (!ReadCaptureHeader(in, header))
			return false;
		if (outHeader)
			*outHeader = header;

		bool haveTimingReference = false;
		uint64_t prevRecvUs = 0;

		for (;;)
		{
			CaptureRecord record;
			if (!ReadCaptureRecord(in, record))
			{
				// Clean end or a truncated trailing record - either way,
				// not an error (matches the B1 reader's own tolerance).
				if (!config.loop)
					return true;

				in.clear(); // clear eof/fail bits before seeking back
				in.seekg(0, std::ios::beg);
				CaptureHeaderInfo rewindHeader;
				if (!in || !ReadCaptureHeader(in, rewindHeader))
					return true; // can't rewind (non-seekable / re-read failed) - stop, not an error
				haveTimingReference = false;
				continue;
			}

			if (config.timing == ReplayTiming::Realtime)
			{
				if (haveTimingReference && record.recv_wallclock_us > prevRecvUs && config.speed > 0.0)
				{
					double deltaSeconds = (double)(record.recv_wallclock_us - prevRecvUs) / 1000000.0;
					double sleepSeconds = deltaSeconds / config.speed;
					std::this_thread::sleep_for(std::chrono::duration<double>(sleepSeconds));
				}
				prevRecvUs = record.recv_wallclock_us;
				haveTimingReference = true;
			}

			Frame frame;
			uint64_t seq = 0, wallclockUs = 0;
			uint32_t epoch = 0;
			if (SubjectList::PeekMeta(record.wire_bytes.data(), record.wire_bytes.size(), seq, wallclockUs, epoch))
			{
				frame.seq = seq;
				frame.wallclock_us = wallclockUs;
				frame.epoch = epoch;
			}
			// else: leave seq/wallclock_us/epoch at Frame's own defaults
			// (0) - not a valid SubjectList buffer, so treated exactly
			// like a legacy/unset frame rather than dropped.

			double captureTimeSeconds = (double)record.recv_wallclock_us / 1000000.0;
			frame.bytes = std::move(record.wire_bytes);

			if (!sink(std::move(frame), captureTimeSeconds))
				return true; // caller asked us to stop
		}
	}
}
