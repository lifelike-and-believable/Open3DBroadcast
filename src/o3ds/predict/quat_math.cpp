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

#include "quat_math.h"

#include <algorithm>
#include <cmath>

namespace O3DS
{
	Quat QuatMultiply(const Quat& a, const Quat& b)
	{
		const double ax = a.v[0], ay = a.v[1], az = a.v[2], aw = a.v[3];
		const double bx = b.v[0], by = b.v[1], bz = b.v[2], bw = b.v[3];

		return Quat(
			aw * bx + ax * bw + ay * bz - az * by,
			aw * by - ax * bz + ay * bw + az * bx,
			aw * bz + ax * by - ay * bx + az * bw,
			aw * bw - ax * bx - ay * by - az * bz);
	}

	Quat QuatConjugate(const Quat& q)
	{
		return Quat(-q.v[0], -q.v[1], -q.v[2], q.v[3]);
	}

	Quat QuatNormalize(const Quat& q)
	{
		const double lenSq = q.v[0] * q.v[0] + q.v[1] * q.v[1] + q.v[2] * q.v[2] + q.v[3] * q.v[3];
		if (lenSq < 1.0e-18) return QuatIdentity();

		const double invLen = 1.0 / std::sqrt(lenSq);
		return Quat(q.v[0] * invLen, q.v[1] * invLen, q.v[2] * invLen, q.v[3] * invLen);
	}

	bool QuatToAxisAngle(const Quat& q, Vector3d& outAxis, double& outAngleRad)
	{
		// Pick the representative (q or -q, same rotation) with w >= 0 so
		// the recovered angle is always the shortest-path magnitude in
		// [0, pi], not "the long way around".
		Quat qn = q;
		if (qn.v[3] < 0.0)
		{
			qn = Quat(-qn.v[0], -qn.v[1], -qn.v[2], -qn.v[3]);
		}

		double w = qn.v[3];
		w = std::max(-1.0, std::min(1.0, w));

		const double angle = 2.0 * std::acos(w);
		const double s = std::sqrt(std::max(0.0, 1.0 - w * w)); // sin(angle/2)

		if (s < 1.0e-9)
		{
			// Near-identity rotation: axis is undefined.
			return false;
		}

		outAxis = Vector3d(qn.v[0] / s, qn.v[1] / s, qn.v[2] / s);
		outAngleRad = angle;
		return true;
	}

	Quat QuatFromAxisAngle(const Vector3d& axis, double angleRad)
	{
		const double lenSq = axis.v[0] * axis.v[0] + axis.v[1] * axis.v[1] + axis.v[2] * axis.v[2];
		if (lenSq < 1.0e-18) return QuatIdentity();

		const double invLen = 1.0 / std::sqrt(lenSq);
		const double half = angleRad * 0.5;
		const double s = std::sin(half);

		return Quat(axis.v[0] * invLen * s, axis.v[1] * invLen * s, axis.v[2] * invLen * s, std::cos(half));
	}
}
