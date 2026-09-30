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

#include "sender_sync.h"

#include <algorithm>
#include <cmath>

namespace O3DS
{
	bool ConsumeCaptureBudget(double nowSeconds, double& inOutLastSlot, double rateHz, double toleranceSeconds)
	{
		// !(rateHz > 0) also catches NaN.
		if (!(rateHz > 0.0))
		{
			inOutLastSlot = nowSeconds;
			return true;
		}

		if (inOutLastSlot <= 0.0)
		{
			inOutLastSlot = nowSeconds;
			return true;
		}

		const double interval = 1.0 / rateHz;
		const double tolerance = std::min(std::max(0.0, toleranceSeconds), 0.5 * interval);
		const double elapsed = nowSeconds - inOutLastSlot;

		// Clock stepped backwards by more than an interval: re-anchor rather
		// than refusing every tick until the old schedule is reached again.
		if (elapsed < -interval)
		{
			inOutLastSlot = nowSeconds;
			return true;
		}

		if (elapsed + tolerance < interval)
		{
			return false;
		}

		inOutLastSlot += interval;
		if (nowSeconds - inOutLastSlot >= interval)
		{
			// One or more whole intervals behind: re-anchor so a hitch or a
			// tick rate below the capture rate doesn't turn into catch-up.
			inOutLastSlot = nowSeconds;
		}
		return true;
	}

	bool FilterCurveValue(float& inOutValue, bool hasLastSent, float lastSent, float epsilon, float deltaThreshold)
	{
		const bool valueIsZero = std::fabs(inOutValue) < epsilon;
		if (valueIsZero)
		{
			if (!hasLastSent || std::fabs(lastSent) < epsilon)
			{
				return false;
			}
			// Returning to rest: send an exact zero once, so the receiver
			// doesn't keep the last non-zero value.
			inOutValue = 0.0f;
			return true;
		}

		if (hasLastSent && std::fabs(inOutValue - lastSent) < deltaThreshold)
		{
			return false;
		}
		return true;
	}

	uint32_t FullSyncTracker::Evaluate(const FullSyncInputs& in) const
	{
		if (!mHasSent)
		{
			return First;
		}

		uint32_t reasons = None;
		if (mRequested)
		{
			reasons |= Requested;
		}
		if (in.descriptorHash != mDescriptorHash)
		{
			reasons |= Descriptor;
		}
		if (in.curveNamesHash != mCurveNamesHash)
		{
			reasons |= CurveNames;
		}
		if (in.encodingFingerprint != mEncodingFingerprint)
		{
			reasons |= Encoding;
		}
		if (in.intervalSeconds > 0.0)
		{
			const double sinceFull = in.nowSeconds - mLastFullSeconds;
			// A clock that went backwards (sinceFull < 0) can't prove the
			// interval hasn't elapsed, so it counts as elapsed.
			if (sinceFull < 0.0 || sinceFull >= in.intervalSeconds)
			{
				reasons |= Interval;
			}
		}
		return reasons;
	}

	void FullSyncTracker::MarkFullSent(const FullSyncInputs& in)
	{
		mHasSent = true;
		mRequested = false;
		mDescriptorHash = in.descriptorHash;
		mCurveNamesHash = in.curveNamesHash;
		mEncodingFingerprint = in.encodingFingerprint;
		mLastFullSeconds = in.nowSeconds;
	}

	void FullSyncTracker::Reset()
	{
		*this = FullSyncTracker();
	}
}
