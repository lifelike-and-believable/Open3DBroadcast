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

#include "channel_quant.h"

#include <algorithm>
#include <cmath>

namespace O3DS
{
	namespace
	{
		constexpr double kByteMax = 127.0;
		constexpr double kHalfMax = 32767.0;

		// Standard "smallest three" bound: for a unit quaternion, if the
		// largest-magnitude component is dropped, each of the remaining
		// three is bounded by 1/sqrt(2). Derivation: the dropped component
		// d is the max of 4 non-negative squares summing to 1, so d^2 >=
		// 1/4. For any other component c, c^2 <= d^2 (d is the max), and
		// d^2 + c^2 <= 1 (both are among the 4 squares summing to 1), so
		// 2*c^2 <= d^2 + c^2 <= 1, giving |c| <= 1/sqrt(2).
		const double kSmallestThreeBound = 1.0 / std::sqrt(2.0);

		int8_t ClampToByteCode(double raw)
		{
			double clamped = std::max(-kByteMax, std::min(kByteMax, raw));
			return static_cast<int8_t>(std::lround(clamped));
		}

		int16_t ClampToHalfCode(double raw)
		{
			double clamped = std::max(-kHalfMax, std::min(kHalfMax, raw));
			return static_cast<int16_t>(std::lround(clamped));
		}
	}

	QuantTier ChooseScalarTier(double absDelta, const QuantRanges& ranges)
	{
		if (absDelta <= ranges.byteRange)
		{
			return QuantTier::Byte;
		}
		if (absDelta <= ranges.halfRange)
		{
			return QuantTier::Half;
		}
		return QuantTier::Full;
	}

	int8_t QuantizeByte(double delta, double range)
	{
		if (range <= 0.0)
		{
			return 0;
		}
		return ClampToByteCode((delta / range) * kByteMax);
	}

	double DequantizeByte(int8_t code, double range)
	{
		return (static_cast<double>(code) / kByteMax) * range;
	}

	int16_t QuantizeHalf(double delta, double range)
	{
		if (range <= 0.0)
		{
			return 0;
		}
		return ClampToHalfCode((delta / range) * kHalfMax);
	}

	double DequantizeHalf(int16_t code, double range)
	{
		return (static_cast<double>(code) / kHalfMax) * range;
	}

	namespace
	{
		// Shared encode-side logic for both smallest-three tiers: finds the
		// largest-magnitude component, sign-normalizes so it's positive
		// (q and -q are the same rotation), and returns its index plus the
		// other three components in ascending index order.
		void PrepareSmallestThree(const Quat& q, uint8_t& outDroppedIndex, double outOthers[3])
		{
			int maxIdx = 0;
			double maxAbs = std::fabs(q.v[0]);
			for (int i = 1; i < 4; ++i)
			{
				double a = std::fabs(q.v[i]);
				if (a > maxAbs)
				{
					maxAbs = a;
					maxIdx = i;
				}
			}

			double sign = (q.v[maxIdx] < 0.0) ? -1.0 : 1.0;

			int outIdx = 0;
			for (int i = 0; i < 4; ++i)
			{
				if (i == maxIdx)
				{
					continue;
				}
				outOthers[outIdx++] = q.v[i] * sign;
			}
			outDroppedIndex = static_cast<uint8_t>(maxIdx);
		}

		// Shared decode-side logic: reconstructs the dropped component from
		// the unit-length constraint (always non-negative by construction -
		// see PrepareSmallestThree's sign normalization) and reassembles the
		// full quaternion, then renormalizes to absorb quantization error
		// (the roadmap's "decode always renormalizes cleanly" requirement).
		Quat ReconstructSmallestThree(uint8_t droppedIndex, double a, double b, double c)
		{
			// droppedIndex is untrusted network input (wire byte, see
			// RotationUpdateQ8/16 in o3ds.fbs) - a value outside [0,3] would
			// otherwise never match the `i == droppedIndex` check below, so
			// the loop would read others[3] on its 4th iteration, one past
			// the end of the 3-element array. Clamp defensively rather than
			// trust it.
			if (droppedIndex > 3)
			{
				droppedIndex = 3;
			}

			double sumSq = a * a + b * b + c * c;
			double dropped = std::sqrt(std::max(0.0, 1.0 - sumSq));

			double comp[4];
			int inIdx = 0;
			double others[3] = { a, b, c };
			for (int i = 0; i < 4; ++i)
			{
				if (i == droppedIndex)
				{
					comp[i] = dropped;
				}
				else
				{
					comp[i] = others[inIdx++];
				}
			}

			return QuatNormalize(Quat(comp[0], comp[1], comp[2], comp[3]));
		}
	}

	SmallestThreeQ8 QuantizeRotationByte(const Quat& q)
	{
		SmallestThreeQ8 out;
		double others[3];
		PrepareSmallestThree(q, out.droppedIndex, others);
		out.a = ClampToByteCode((others[0] / kSmallestThreeBound) * kByteMax);
		out.b = ClampToByteCode((others[1] / kSmallestThreeBound) * kByteMax);
		out.c = ClampToByteCode((others[2] / kSmallestThreeBound) * kByteMax);
		return out;
	}

	Quat DequantizeRotationByte(const SmallestThreeQ8& q)
	{
		double a = (static_cast<double>(q.a) / kByteMax) * kSmallestThreeBound;
		double b = (static_cast<double>(q.b) / kByteMax) * kSmallestThreeBound;
		double c = (static_cast<double>(q.c) / kByteMax) * kSmallestThreeBound;
		return ReconstructSmallestThree(q.droppedIndex, a, b, c);
	}

	SmallestThreeQ16 QuantizeRotationHalf(const Quat& q)
	{
		SmallestThreeQ16 out;
		double others[3];
		PrepareSmallestThree(q, out.droppedIndex, others);
		out.a = ClampToHalfCode((others[0] / kSmallestThreeBound) * kHalfMax);
		out.b = ClampToHalfCode((others[1] / kSmallestThreeBound) * kHalfMax);
		out.c = ClampToHalfCode((others[2] / kSmallestThreeBound) * kHalfMax);
		return out;
	}

	Quat DequantizeRotationHalf(const SmallestThreeQ16& q)
	{
		double a = (static_cast<double>(q.a) / kHalfMax) * kSmallestThreeBound;
		double b = (static_cast<double>(q.b) / kHalfMax) * kSmallestThreeBound;
		double c = (static_cast<double>(q.c) / kHalfMax) * kSmallestThreeBound;
		return ReconstructSmallestThree(q.droppedIndex, a, b, c);
	}
}
