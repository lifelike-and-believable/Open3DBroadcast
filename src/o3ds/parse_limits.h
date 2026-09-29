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
}

#endif
