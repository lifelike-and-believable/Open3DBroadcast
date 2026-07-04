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

#ifndef O3DS_PREDICT_QUAT_MATH_H
#define O3DS_PREDICT_QUAT_MATH_H

#include "../math.h"

namespace O3DS
{
	//! x,y,z,w unit-quaternion convention, matching Vector4d's existing use
	//! for rotations elsewhere in this codebase (see Transform::rotation).
	using Quat = Vector4d;

	inline Quat QuatIdentity() { return Quat(0.0, 0.0, 0.0, 1.0); }

	//! Hamilton product: applying (a*b) to a vector is equivalent to
	//! applying b first, then a.
	Quat QuatMultiply(const Quat& a, const Quat& b);

	//! (x,y,z,w) -> (-x,-y,-z,w); the inverse of a unit quaternion.
	Quat QuatConjugate(const Quat& q);

	//! Returns q / |q|; falls back to the identity quaternion if |q| is
	//! degenerate (near zero) rather than dividing by ~0.
	Quat QuatNormalize(const Quat& q);

	//! Decomposes a unit quaternion into an axis (unit vector) and angle
	//! (radians, [0, pi] - always the shortest-path magnitude, since q and
	//! -q represent the same rotation and this picks whichever has w >= 0
	//! before decomposing). Returns false (axis/angle left untouched) if q
	//! is within epsilon of the identity - there's no well-defined axis for
	//! a near-zero rotation.
	bool QuatToAxisAngle(const Quat& q, Vector3d& outAxis, double& outAngleRad);

	//! Builds a unit quaternion representing a rotation of angleRad radians
	//! about axis (which need not be pre-normalized). Returns the identity
	//! quaternion if axis is near-zero-length.
	Quat QuatFromAxisAngle(const Vector3d& axis, double angleRad);
}

#endif
