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

#ifndef O3DS_PREDICT_SAMPLE_RING_H
#define O3DS_PREDICT_SAMPLE_RING_H

#include <array>
#include <cstddef>

#include "pose_predictor.h"

namespace O3DS
{
	//! Fixed-capacity ring of the N most-recently Observe()'d samples,
	//! oldest-to-newest ordered access via operator[]. All N slots are
	//! preallocated up front, so steady-state Push() calls overwrite
	//! existing PoseSample storage via assignment - typical std::vector
	//! implementations reuse each member vector's already-allocated
	//! capacity when the new sample's channel counts match rather than
	//! reallocating, keeping this allocation-light after warm-up per the
	//! roadmap's C0 requirement.
	template <size_t N>
	class SampleRing
	{
		static_assert(N > 0, "SampleRing requires N > 0");

	public:
		void Push(const PoseSample& sample)
		{
			mSlots[mNext] = sample;
			mNext = (mNext + 1) % N;
			if (mCount < N) ++mCount;
		}

		void Clear()
		{
			mCount = 0;
			mNext = 0;
		}

		size_t Count() const { return mCount; }

		//! index 0 = oldest retained sample, Count()-1 = newest.
		const PoseSample& operator[](size_t index) const
		{
			const size_t start = (mNext + N - mCount) % N;
			return mSlots[(start + index) % N];
		}

	private:
		std::array<PoseSample, N> mSlots;
		size_t mNext = 0;
		size_t mCount = 0;
	};
}

#endif
