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

#include "quadratic_predictor.h"

#include <algorithm>

namespace O3DS
{
	namespace
	{
		constexpr double kMinDtSeconds = 1.0e-6;

		// Newton divided-difference quadratic extrapolation through
		// (t0,x0), (t1,x1), (t2,x2), evaluated at t. Correct for unevenly
		// spaced samples (unlike assuming a fixed frame interval).
		double QuadraticExtrapolate(double t0, double x0, double t1, double x1, double t2, double x2, double t)
		{
			const double d01 = (x1 - x0) / (t1 - t0);
			const double d12 = (x2 - x1) / (t2 - t1);
			const double d012 = (d12 - d01) / (t2 - t0);
			return x0 + d01 * (t - t0) + d012 * (t - t0) * (t - t1);
		}
	}

	void QuadraticPredictor::Observe(const PoseSample& sample)
	{
		mHistory.Push(sample);
	}

	bool QuadraticPredictor::Predict(double t, PoseSample& out) const
	{
		if (mHistory.Count() < 3) return false;

		const PoseSample& s0 = mHistory[0];
		const PoseSample& s1 = mHistory[1];
		const PoseSample& s2 = mHistory[2];

		const double dtA = s1.t - s0.t;
		const double dtB = s2.t - s1.t;

		if (!(dtA > kMinDtSeconds) || !(dtB > kMinDtSeconds))
		{
			// Degenerate spacing: fall back to holding the newest sample.
			out = s2;
			out.t = t;
			return true;
		}

		out.t = t;
		out.seq = s2.seq;

		const size_t nTrans = std::min({ s0.translations.size(), s1.translations.size(), s2.translations.size() });
		out.translations.resize(nTrans);
		for (size_t i = 0; i < nTrans; ++i)
		{
			for (int c = 0; c < 3; ++c)
			{
				out.translations[i].v[c] = QuadraticExtrapolate(
					s0.t, s0.translations[i].v[c],
					s1.t, s1.translations[i].v[c],
					s2.t, s2.translations[i].v[c], t);
			}
		}

		const size_t nScale = std::min({ s0.scales.size(), s1.scales.size(), s2.scales.size() });
		out.scales.resize(nScale);
		for (size_t i = 0; i < nScale; ++i)
		{
			for (int c = 0; c < 3; ++c)
			{
				out.scales[i].v[c] = QuadraticExtrapolate(
					s0.t, s0.scales[i].v[c],
					s1.t, s1.scales[i].v[c],
					s2.t, s2.scales[i].v[c], t);
			}
		}

		const size_t nCurves = std::min({ s0.curves.size(), s1.curves.size(), s2.curves.size() });
		out.curves.resize(nCurves);
		for (size_t i = 0; i < nCurves; ++i)
		{
			out.curves[i] = static_cast<float>(QuadraticExtrapolate(
				s0.t, s0.curves[i], s1.t, s1.curves[i], s2.t, s2.curves[i], t));
		}

		const size_t nRot = std::min({ s0.rotations.size(), s1.rotations.size(), s2.rotations.size() });
		out.rotations.resize(nRot);
		for (size_t i = 0; i < nRot; ++i)
		{
			const Quat& q0 = s0.rotations[i];
			const Quat& q1 = s1.rotations[i];
			const Quat& q2 = s2.rotations[i];

			const Quat dqA = QuatMultiply(q1, QuatConjugate(q0)); // rotation from q0 to q1
			const Quat dqB = QuatMultiply(q2, QuatConjugate(q1)); // rotation from q1 to q2

			Vector3d axisA, axisB;
			double angleA = 0.0, angleB = 0.0;
			const bool hasA = QuatToAxisAngle(dqA, axisA, angleA);
			const bool hasB = QuatToAxisAngle(dqB, axisB, angleB);

			if (!hasB)
			{
				// No meaningful recent rotation - hold q2.
				out.rotations[i] = q2;
				continue;
			}

			const double omega2 = angleB / dtB; // mean angular speed over [t1,t2], about axisB

			double alpha = 0.0;
			if (hasA)
			{
				// Only the *magnitude* of the older delta's angular speed
				// feeds the acceleration estimate - see class doc comment
				// on why axisA's direction isn't used.
				const double omega1 = angleA / dtA;
				alpha = (omega2 - omega1) / ((s2.t - s0.t) * 0.5);
			}

			// omega2 is the mean speed over [t1,t2], i.e. the instantaneous
			// speed at the interval midpoint (t1 + dtB/2), not at t2. Advance
			// it by half the interval under the constant-alpha model to get
			// the instantaneous speed at t2 before extrapolating further.
			const double omegaAtT2 = omega2 + alpha * (dtB * 0.5);

			const double dtExtrap = t - s2.t;
			const double thetaExtrap = omegaAtT2 * dtExtrap + 0.5 * alpha * dtExtrap * dtExtrap;

			const Quat dqExtrap = QuatFromAxisAngle(axisB, thetaExtrap);
			out.rotations[i] = QuatNormalize(QuatMultiply(dqExtrap, q2));
		}

		return true;
	}

	void QuadraticPredictor::Reset()
	{
		mHistory.Clear();
	}
}
