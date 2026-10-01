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

#ifndef OPEN3D_STREAM_PARSE_LIMITS_H
#define OPEN3D_STREAM_PARSE_LIMITS_H

#include <cstddef>
#include <cstdint>

namespace O3DS
{
	//! Hard upper bounds SubjectList::Parse() enforces on untrusted wire
	//! buffers (WP-S1, findings CORE-8/CORE-9).
	//!
	//! The FlatBuffers Verifier only proves a buffer is structurally sound;
	//! it happily accepts millions of tiny tables. Each parsed node becomes a
	//! heap Transform (~400 B) and costs matrix work on every Parse(), so
	//! without these caps a single large datagram can expand into GB-scale
	//! allocation and seconds of CPU on the receiver.
	//!
	//! The defaults are deliberately generous: a production full-body rig
	//! (body + hands + face joints) is a few hundred transforms, and an ARKit
	//! or MetaHuman face is a few hundred curves, so real senders sit an
	//! order of magnitude below every limit. A buffer that exceeds any of
	//! them is rejected as a whole (Parse() returns false with mError set)
	//! rather than truncated, so a receiver never silently shows a partial
	//! skeleton.
	namespace ParseLimits
	{
		//! Max entries in one buffer's `subjects` vector, in its `updates`
		//! vector, and max subjects one SubjectList holds after a Parse()
		//! (the last matters when Parse() is called with clearInactive=false
		//! and subjects accumulate across buffers).
		constexpr std::size_t kMaxSubjects = 256;

		//! Max transforms (nodes) in one subject.
		constexpr std::size_t kMaxTransformsPerSubject = 4096;

		//! Max curves in one subject.
		constexpr std::size_t kMaxCurvesPerSubject = 4096;

		//! Max entries in one transform's `components` vector, and max
		//! entries in its `matrix` vector. Real transforms carry a handful
		//! (translate/rotate/scale plus a few pivot/offset matrices).
		constexpr std::size_t kMaxComponentsPerTransform = 64;
	}

	//! Hard upper bounds O3DS::Control::Validate() enforces on control
	//! messages (docs/adr/0011-control-channel.md, decision items 2, 4 and 9),
	//! on the reading side and, for the same values, the writing side.
	namespace ControlLimits
	{
		//! Largest control envelope on the wire, header included. Fits one
		//! UDP datagram below the fragmentation threshold and the WebRTC
		//! lossy data-channel limit (~1300 bytes), so control is never
		//! fragmented on any transport.
		constexpr std::size_t kMaxEnvelopeBytes = 1100;

		//! Envelope header the budget reserves: the larger of envelope v1
		//! (20 bytes) and v2 (24 bytes, ADR 0009 item 4).
		constexpr std::size_t kEnvelopeHeaderBytes = 24;

		//! Largest ControlMessage FlatBuffer.
		constexpr std::size_t kMaxPayloadBytes = kMaxEnvelopeBytes - kEnvelopeHeaderBytes;

		//! Max set + clear + events entries in one message.
		constexpr std::size_t kMaxItemsPerMessage = 64;

		//! Max UTF-8 bytes in a key or event name.
		constexpr std::size_t kMaxKeyBytes = 128;

		//! Max UTF-8 bytes in a target subject name.
		constexpr std::size_t kMaxTargetBytes = 128;

		//! Max UTF-8 bytes in a String or Name value.
		constexpr std::size_t kMaxStringValueBytes = 512;

		//! Max bytes in a Bytes value.
		constexpr std::size_t kMaxBytesValueBytes = 512;

		//! Max bytes in source_id (a GUID string is 32-38).
		constexpr std::size_t kMaxSourceIdBytes = 64;

		//! Max UTF-8 bytes in source_name.
		constexpr std::size_t kMaxSourceNameBytes = 128;

		//! Max entries in mocap_subjects, and max UTF-8 bytes in each.
		constexpr std::size_t kMaxMocapSubjects = 16;
		constexpr std::size_t kMaxMocapSubjectBytes = 128;

		//! Max UTF-8 bytes of mocap_subjects names together; the publisher
		//! drops trailing names beyond this so the header leaves room for
		//! items.
		constexpr std::size_t kMaxMocapSubjectsTotalBytes = 256;

		//! Max parts one snapshot is split into. Every entry fits a part on
		//! its own, so this must be at least the largest key table
		//! (kMaxKeysPerSource).
		constexpr std::size_t kMaxSnapshotParts = 2048;

		//! Largest value table a publisher keeps and the receiver default.
		constexpr std::size_t kMaxKeysPerSource = 1024;

		//! Max event time-to-live a message may ask for.
		constexpr std::uint32_t kMaxEventTtlMs = 60000;

		//! Publisher output caps, per source. Each is half the receiver's
		//! default budget for the same class, so a publisher at full output
		//! (including its one-second burst) never trips a default receiver's
		//! rate limit, and snapshots always complete (ADR 0011 item 9).
		constexpr double kPublisherMaxLiveBytesPerS = 32.0 * 1024.0;
		constexpr double kPublisherMaxSnapshotBytesPerS = 256.0 * 1024.0;
		constexpr double kReceiverDefaultLiveBytesPerS = 2.0 * kPublisherMaxLiveBytesPerS;
		constexpr double kReceiverDefaultSnapshotBytesPerS = 2.0 * kPublisherMaxSnapshotBytesPerS;

		static_assert(kMaxSnapshotParts >= kMaxKeysPerSource, "a full table must fit one snapshot");
		static_assert(kPublisherMaxLiveBytesPerS * 2.0 <= kReceiverDefaultLiveBytesPerS, "publisher live burst must fit the receiver bucket");
		static_assert(kPublisherMaxSnapshotBytesPerS * 2.0 <= kReceiverDefaultSnapshotBytesPerS, "publisher snapshot burst must fit the receiver bucket");
	}
}

#endif
