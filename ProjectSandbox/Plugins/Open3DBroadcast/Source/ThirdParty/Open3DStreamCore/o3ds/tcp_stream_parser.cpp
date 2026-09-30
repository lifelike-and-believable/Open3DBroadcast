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

#include "tcp_stream_parser.h"

#include <algorithm>
#include <cstring>

namespace O3DS
{
	const uint8_t kTcpFrameMagic[kTcpFrameMagicSize] =
	{
		0x00, 0xFF, 0x03, 0xFE, 'O', '3', 'D', 'S', '-', 'S', 'T', 'A', 'R', 'T'
	};

	void writeTcpFrameHeader(uint8_t* out, uint32_t payloadSize)
	{
		std::memcpy(out, kTcpFrameMagic, kTcpFrameMagicSize);
		out[kTcpFrameMagicSize + 0] = static_cast<uint8_t>(payloadSize & 0xFF);
		out[kTcpFrameMagicSize + 1] = static_cast<uint8_t>((payloadSize >> 8) & 0xFF);
		out[kTcpFrameMagicSize + 2] = static_cast<uint8_t>((payloadSize >> 16) & 0xFF);
		out[kTcpFrameMagicSize + 3] = static_cast<uint8_t>((payloadSize >> 24) & 0xFF);
	}

	namespace
	{
		uint32_t readLE32(const uint8_t* p)
		{
			return static_cast<uint32_t>(p[0]) |
				(static_cast<uint32_t>(p[1]) << 8) |
				(static_cast<uint32_t>(p[2]) << 16) |
				(static_cast<uint32_t>(p[3]) << 24);
		}

		// Buffers above this are released once the live data fits in
		// kRetainCapacity again. The gap to kRetainCapacity is hysteresis, so
		// a stream of medium frames does not reallocate on every frame.
		const size_t kShrinkThreshold = 4 * TcpStreamParser::kRetainCapacity;
	}

	TcpStreamParser::TcpStreamParser(size_t maxPayloadBytes)
	{
		setMaxPayloadBytes(maxPayloadBytes);
	}

	void TcpStreamParser::setMaxPayloadBytes(size_t maxPayloadBytes)
	{
		mMaxPayload = std::min(std::max<size_t>(maxPayloadBytes, 1), kTcpMaxPayloadLimit);
	}

	uint8_t* TcpStreamParser::prepareWrite(size_t minBytes)
	{
		if (mRead == mWrite)
		{
			mRead = 0;
			mWrite = 0;
		}

		const size_t live = mWrite - mRead;
		if (mBuffer.size() > kShrinkThreshold && live + minBytes <= kRetainCapacity)
		{
			std::vector<uint8_t> smaller(kRetainCapacity);
			if (live > 0)
				std::memcpy(smaller.data(), mBuffer.data() + mRead, live);
			mBuffer.swap(smaller);
			mRead = 0;
			mWrite = live;
			return mBuffer.data() + mWrite;
		}

		if (mBuffer.size() - mWrite >= minBytes)
			return mBuffer.data() + mWrite;

		if (mRead > 0)
		{
			std::memmove(mBuffer.data(), mBuffer.data() + mRead, live);
			mRead = 0;
			mWrite = live;
		}

		if (mBuffer.size() - mWrite < minBytes)
		{
			// Geometric growth keeps the copy cost linear in the frame size.
			mBuffer.resize(std::max(mWrite + minBytes, mBuffer.size() * 2));
		}
		return mBuffer.data() + mWrite;
	}

	void TcpStreamParser::commitWrite(size_t count)
	{
		mWrite = std::min(mWrite + count, mBuffer.size());
	}

	void TcpStreamParser::append(const uint8_t* data, size_t size)
	{
		if (!data || size == 0)
			return;
		uint8_t* out = prepareWrite(size);
		std::memcpy(out, data, size);
		commitWrite(size);
	}

	void TcpStreamParser::discard(size_t count)
	{
		if (count == 0)
			return;
		if (!mInGarbageRun)
		{
			mInGarbageRun = true;
			++mStats.resyncs;
		}
		mStats.discardedBytes += count;
		mRead += count;
	}

	bool TcpStreamParser::alignToMagic()
	{
		const size_t avail = mWrite - mRead;
		if (avail == 0)
			return false;

		// First offset whose bytes match the magic, or a prefix of it when
		// the buffer ends before a whole magic. Everything before it cannot
		// start a frame and is dropped in this one pass.
		const uint8_t* p = mBuffer.data() + mRead;
		size_t i = 0;
		while (i < avail)
		{
			const void* hit = std::memchr(p + i, kTcpFrameMagic[0], avail - i);
			if (!hit)
			{
				i = avail;
				break;
			}
			i = static_cast<size_t>(static_cast<const uint8_t*>(hit) - p);
			const size_t n = std::min(kTcpFrameMagicSize, avail - i);
			if (std::memcmp(p + i, kTcpFrameMagic, n) == 0)
				break;
			++i;
		}

		discard(i);
		return buffered() >= kTcpFrameHeaderSize;
	}

	bool TcpStreamParser::next(const uint8_t*& payload, size_t& size)
	{
		for (;;)
		{
			if (!alignToMagic())
				return false;

			const uint8_t* header = mBuffer.data() + mRead;
			const size_t length = readLE32(header + kTcpFrameMagicSize);
			if (length == 0 || length > mMaxPayload)
			{
				// Not a frame we accept: drop the magic's first byte so the
				// next alignToMagic() scans past this header (TRB-8, TRB-9).
				++mStats.rejectedFrames;
				discard(1);
				continue;
			}

			if (buffered() < kTcpFrameHeaderSize + length)
				return false;

			payload = header + kTcpFrameHeaderSize;
			size = length;
			mRead += kTcpFrameHeaderSize + length;
			++mStats.frames;
			mInGarbageRun = false;
			return true;
		}
	}

	void TcpStreamParser::reset()
	{
		std::vector<uint8_t>().swap(mBuffer);
		mRead = 0;
		mWrite = 0;
		mInGarbageRun = false;
	}
}
