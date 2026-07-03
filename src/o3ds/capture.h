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

#ifndef OPEN3D_STREAM_CAPTURE_H
#define OPEN3D_STREAM_CAPTURE_H

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace O3DS
{
	//! .o3dscap v1 container format (see docs/roadmap/resilient-streaming-
	//! and-motion-prediction.md, Phase B1, for the full byte-level spec).
	//! Captures verbatim on-the-wire O3DS frames plus their receive time, so
	//! writing a capture is near-zero cost (no re-encoding) and safe to call
	//! inline on a hot send/receive path.
	//!
	//! All multi-byte fields are little-endian on the wire regardless of
	//! host byte order (encoded/decoded explicitly, not via raw memcpy).
	//! These functions operate on std::istream&/std::ostream& - any
	//! seekable stream works (std::ifstream/ofstream for real files,
	//! std::istringstream/ostringstream for tests); ReadCaptureHeader
	//! requires the input stream to be seekable (see its doc comment).
	//!
	//! IMPORTANT for real files: open with std::ios::binary. This is a
	//! binary format; a file stream opened in text mode can silently
	//! rewrite byte sequences in the payload (historically, and still
	//! today on Windows, 0x0A <-> 0x0D 0x0A translation), corrupting
	//! captured frames without any read/write call reporting failure.

	static const uint16_t kCaptureFormatVersion = 1;

	//! Reject any single record's wire_bytes above this size before
	//! allocating for it - guards against a corrupt/malicious wire_len
	//! (a uint32 field, so its face-value range goes well past what any
	//! real O3DS frame should be) causing a huge allocation. Mirrors the
	//! same posture already applied to the network-facing parsers.
	static const uint32_t kCaptureMaxWireLen = 64u * 1024u * 1024u; // 64MB

	struct CaptureHeaderInfo
	{
		uint32_t flags = 0;              // bit0 = timestamps are UTC-us; other bits reserved (0)
		uint64_t base_wallclock_us = 0;  // capture start; 0 = unset
		uint32_t schema_fingerprint = 0; // e.g. a hash of o3ds.fbs/O3DS_VERSION_TAG; 0 = unset
		uint32_t predictor_version = 0;  // 0 = none (Workstream C)
		std::string source_desc;         // free-form UTF-8 (transport, host, notes); max 65501 bytes
		                                  // (65535 minus the 34-byte fixed header, so header_len itself
		                                  // still fits in its own uint16 field)
	};

	struct CaptureRecord
	{
		uint64_t recv_wallclock_us = 0; // capture/receive time, absolute
		std::vector<char> wire_bytes;   // the exact on-the-wire O3DS frame, verbatim
	};

	//! Writes the .o3dscap header. Call exactly once, before any
	//! WriteCaptureRecord calls on the same stream. Returns false if
	//! `info.source_desc` exceeds 65535 bytes (its wire length prefix is a
	//! uint16) or the stream write fails.
	bool WriteCaptureHeader(std::ostream& out, const CaptureHeaderInfo& info);

	//! Appends one record. Returns false if `record.wire_bytes.size()`
	//! exceeds kCaptureMaxWireLen (a byte count that large should never be
	//! reached by a real O3DS frame; this is a guard, not a realistic
	//! capture) or the stream write fails.
	bool WriteCaptureRecord(std::ostream& out, const CaptureRecord& record);

	//! Reads and validates the header: magic, and format_version (v1
	//! readers accept only format_version == 1; a future version may
	//! introduce major/minor semantics). Requires `in` to be seekable
	//! (uses seekg to skip to the end of the header, including any
	//! reserved padding a newer writer may have added) - real files and
	//! std::istringstream both satisfy this; a non-seekable stream is not
	//! supported. Returns false on any validation failure or short read;
	//! `outInfo` is left default-constructed in that case.
	bool ReadCaptureHeader(std::istream& in, CaptureHeaderInfo& outInfo);

	//! Reads the next record. Returns false when there isn't a complete
	//! record left to read - this covers BOTH a clean end-of-capture and a
	//! truncated trailing record (a capture cut mid-write) identically, by
	//! design: a truncated tail is not an error, just the end of what was
	//! successfully captured. Never throws or reads out of bounds on
	//! malformed/truncated/adversarial input. `outRecord` is left
	//! unspecified (do not use) when this returns false.
	bool ReadCaptureRecord(std::istream& in, CaptureRecord& outRecord);
}

#endif
