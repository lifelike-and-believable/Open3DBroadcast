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

#ifndef OPEN3D_STREAM_TCP_STREAM_PARSER_H
#define OPEN3D_STREAM_TCP_STREAM_PARSER_H

#include "o3ds_export.h"
#include <cstddef>
#include <cstdint>
#include <vector>

// TCP stream framing used by the UE Sockets transport (and the MotionBuilder
// device): a 14-byte magic, a u32 little-endian payload length, then the
// payload. The layout is unchanged from the original UE implementation
// (ADR 0009 item 6); this file only moves the parser out of the socket code
// so it can be unit-tested and fuzzed (ADR 0006 S2, WP-S6).
namespace O3DS
{
	static const size_t kTcpFrameMagicSize = 14;
	static const size_t kTcpFrameHeaderSize = kTcpFrameMagicSize + 4;

	//! Default largest payload a receiver accepts (tcp.maxframe, TRB-9).
	static const size_t kTcpDefaultMaxPayload = 4u * 1024u * 1024u;
	//! Hard upper limit for tcp.maxframe.
	static const size_t kTcpMaxPayloadLimit = 50u * 1024u * 1024u;

	//! The frame magic: 00 FF 03 FE "O3DS-START".
	extern O3DS_API const uint8_t kTcpFrameMagic[kTcpFrameMagicSize];

	//! Writes the 18-byte frame header for a payload of `payloadSize` bytes.
	O3DS_API void writeTcpFrameHeader(uint8_t* out, uint32_t payloadSize);

	struct TcpStreamParserStats
	{
		uint64_t frames = 0;         // complete frames returned by next()
		uint64_t discardedBytes = 0; // bytes skipped while looking for a magic
		uint64_t resyncs = 0;        // runs of discarded bytes (one per garbage run)
		uint64_t rejectedFrames = 0; // headers with a zero or oversize length
	};

	//! Socket-free parser for the TCP frame stream (TRB-1, TRB-8, TRB-9).
	//!
	//! Append received bytes with prepareWrite()/commitWrite() (zero copy
	//! from recv) or append(), then call next() until it returns false. Any
	//! number of frames may arrive in one read, and a header or payload may
	//! be split across reads.
	//!
	//! Resync is a single forward pass: bytes that cannot start a frame are
	//! dropped as the read offset moves past them, and a header with a zero or
	//! oversize length is treated as garbage (its first byte is dropped and
	//! the scan continues). The work per call to next() is O(bytes buffered).
	//!
	//! Memory: the buffer grows only as bytes arrive, never to a length a
	//! header announces, so a hostile length costs nothing until the bytes
	//! are actually sent; it is capped by the max payload. After a large
	//! frame, the buffer is shrunk back to a small retained size.
	//!
	//! Not thread-safe: confine one instance to one thread.
	class O3DS_API TcpStreamParser
	{
	public:
		//! Buffer capacity kept between frames; larger buffers are released.
		static const size_t kRetainCapacity = 256u * 1024u;

		explicit TcpStreamParser(size_t maxPayloadBytes = kTcpDefaultMaxPayload);

		//! Clamped to [1, kTcpMaxPayloadLimit]. Applies to headers parsed later.
		void setMaxPayloadBytes(size_t maxPayloadBytes);
		size_t maxPayloadBytes() const { return mMaxPayload; }

		//! Returns a pointer to at least `minBytes` writable bytes after the
		//! buffered data. Invalidates any payload pointer from next().
		uint8_t* prepareWrite(size_t minBytes);

		//! Marks `count` bytes written at the pointer from prepareWrite().
		//! `count` must not exceed the size requested there.
		void commitWrite(size_t count);

		//! Copies `size` bytes in (convenience for tests and fuzzing).
		void append(const uint8_t* data, size_t size);

		//! Pops the next complete frame. On true, `payload`/`size` point into
		//! the parser's buffer and stay valid until the next call to any
		//! non-const member. Returns false when no complete frame is buffered.
		bool next(const uint8_t*& payload, size_t& size);

		//! Bytes buffered and not yet returned or discarded.
		size_t buffered() const { return mWrite - mRead; }

		//! Current buffer allocation, for tests of the memory bounds.
		size_t capacity() const { return mBuffer.size(); }

		const TcpStreamParserStats& stats() const { return mStats; }

		//! Drops all buffered bytes (e.g. on reconnect) and releases the
		//! buffer. Statistics are kept.
		void reset();

	private:
		//! Moves mRead to the first offset where a frame may start. Returns
		//! false if fewer than a full header is buffered from there.
		bool alignToMagic();
		void discard(size_t count);

		std::vector<uint8_t> mBuffer; // size() is the allocation; [mRead, mWrite) is live
		size_t mRead = 0;
		size_t mWrite = 0;
		size_t mMaxPayload = kTcpDefaultMaxPayload;
		bool mInGarbageRun = false;
		TcpStreamParserStats mStats;
	};
}

#endif
