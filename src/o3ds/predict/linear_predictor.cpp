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

#include "linear_predictor.h"

#include <algorithm>

namespace O3DS
{
	namespace
	{
		// Minimum dt (seconds) below which velocity is considered
		// unreliable/degenerate; falls back to holding the newest sample
		// rather than dividing by ~0.
		constexpr double kMinDtSeconds = 1.0e-6;
	}

	void LinearPredictor::Observe(const PoseSample& sample)
	{
		mHistory.Push(sample);
	}

	bool LinearPredictor::Predict(double t, PoseSample& out) const
	{
		if (mHistory.Count() < 2) return false;

		const PoseSample& s0 = mHistory[0];
		const PoseSample& s1 = mHistory[1];

		const double dt = s1.t - s0.t;
		if (!(dt > kMinDtSeconds))
		{
			out = s1;
			out.t = t;
			return true;
		}

		const double factor = (t - s1.t) / dt;

		out.t = t;
		out.seq = s1.seq;

		const size_t nTrans = std::min(s0.translations.size(), s1.translations.size());
		out.translations.resize(nTrans);
		for (size_t i = 0; i < nTrans; ++i)
		{
			for (int c = 0; c < 3; ++c)
			{
				const double v0 = s0.translations[i].v[c];
				const double v1 = s1.translations[i].v[c];
				out.translations[i].v[c] = v1 + (v1 - v0) * factor;
			}
		}

		const size_t nScale = std::min(s0.scales.size(), s1.scales.size());
		out.scales.resize(nScale);
		for (size_t i = 0; i < nScale; ++i)
		{
			for (int c = 0; c < 3; ++c)
			{
				const double v0 = s0.scales[i].v[c];
				const double v1 = s1.scales[i].v[c];
				out.scales[i].v[c] = v1 + (v1 - v0) * factor;
			}
		}

		const size_t nRot = std::min(s0.rotations.size(), s1.rotations.size());
		out.rotations.resize(nRot);
		for (size_t i = 0; i < nRot; ++i)
		{
			const Quat& q0 = s0.rotations[i];
			const Quat& q1 = s1.rotations[i];

			// Delta rotation such that dq * q0 == q1.
			const Quat dq = QuatMultiply(q1, QuatConjugate(q0));

			Vector3d axis;
			double angle = 0.0;
			if (QuatToAxisAngle(dq, axis, angle))
			{
				const Quat dqScaled = QuatFromAxisAngle(axis, angle * factor);
				out.rotations[i] = QuatNormalize(QuatMultiply(dqScaled, q1));
			}
			else
			{
				// No meaningful rotation between samples - hold q1.
				out.rotations[i] = q1;
			}
		}

		const size_t nCurves = std::min(s0.curves.size(), s1.curves.size());
		out.curves.resize(nCurves);
		for (size_t i = 0; i < nCurves; ++i)
		{
			const float v0 = s0.curves[i];
			const float v1 = s1.curves[i];
			out.curves[i] = static_cast<float>(v1 + (v1 - v0) * factor);
		}

		return true;
	}

	void LinearPredictor::Reset()
	{
		mHistory.Clear();
	}
}
