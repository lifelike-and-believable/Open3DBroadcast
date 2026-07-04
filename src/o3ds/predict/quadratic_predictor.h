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

#ifndef O3DS_PREDICT_QUADRATIC_PREDICTOR_H
#define O3DS_PREDICT_QUADRATIC_PREDICTOR_H

#include "pose_predictor.h"
#include "sample_ring.h"

namespace O3DS
{
	//! Version 2. Constant-acceleration extrapolation from the three most
	//! recent samples for linear/scalar channels (translations, scales,
	//! curves), via Newton divided differences - correct for unevenly
	//! spaced samples. More responsive than LinearPredictor but overshoots
	//! on longer horizons; callers should bound the prediction horizon.
	//!
	//! Rotations use a simplified constant-angular-acceleration model about
	//! the most recent delta rotation's axis (from the newest two
	//! samples); the older delta only contributes its angular *speed* (not
	//! direction) to the acceleration estimate. This is an approximation -
	//! a fully general constant-angular-acceleration model in SO(3) is
	//! materially more complex and not needed for a classical baseline.
	//!
	//! Needs 3 samples with distinct timestamps; Predict() returns false
	//! otherwise (caller should hold).
	class QuadraticPredictor : public IPosePredictor
	{
	public:
		uint32_t Version() const override { return 2; }
		void Observe(const PoseSample& sample) override;
		bool Predict(double t, PoseSample& out) const override;
		void Reset() override;

	private:
		SampleRing<3> mHistory;
	};
}

#endif
