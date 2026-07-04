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

#ifndef O3DS_PREDICT_POSE_PREDICTOR_H
#define O3DS_PREDICT_POSE_PREDICTOR_H

#include <cstdint>
#include <vector>

#include "quat_math.h"

namespace O3DS
{
	//! A flat, topology-stable snapshot of one subject's animatable values.
	//! Channel counts/order are fixed for a subject "epoch" (until a
	//! keyframe changes topology, at which point the caller must Reset()
	//! the predictor); this keeps the predictor pure-math and decoupled
	//! from the FlatBuffers/Subject types, so it unit-tests without the
	//! model layer (roadmap doc, §5/C0). The caller owns the PoseSample <->
	//! O3DS::Subject mapping.
	struct PoseSample
	{
		double   t   = 0.0;   //!< sample time, seconds, in the SENDER clock domain
		uint64_t seq = 0;     //!< tx_seq of the source frame (0 = unset)
		std::vector<Vector3d> translations; //!< per node
		std::vector<Quat>     rotations;    //!< per node (unit quaternion)
		std::vector<Vector3d> scales;       //!< per node
		std::vector<float>    curves;       //!< per curve
	};

	//! Pluggable pose predictor: feed confirmed frames via Observe(), ask
	//! for a pose at an arbitrary time via Predict(). Implementations are
	//! pure-math and allocation-light after warm-up (see each
	//! implementation's own doc comment for its required history depth).
	class IPosePredictor
	{
	public:
		virtual ~IPosePredictor() = default;

		//! A version tag carried in-stream for residual mode (C2) so a
		//! receiver can detect a predictor mismatch and Reset(). Baselines:
		//! HoldPredictor = 0, LinearPredictor = 1, QuadraticPredictor = 2.
		virtual uint32_t Version() const = 0;

		//! Feed one confirmed (real, applied) frame. Samples must arrive in
		//! non-decreasing `t` order - implementations may assume this
		//! rather than re-sorting.
		virtual void Observe(const PoseSample& sample) = 0;

		//! Predict the pose at time t. Returns false (out left untouched)
		//! if there isn't enough history yet - the caller should hold the
		//! last applied pose in that case, not treat false as an error.
		virtual bool Predict(double t, PoseSample& out) const = 0;

		//! Clears all history: call on keyframe/topology change, a version
		//! mismatch, or a large sequence gap (ties to A1's restart
		//! detection). The next Observe() starts a fresh history window.
		virtual void Reset() = 0;
	};
}

#endif
