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

#ifndef O3DS_PREDICT_HOLD_PREDICTOR_H
#define O3DS_PREDICT_HOLD_PREDICTOR_H

#include "pose_predictor.h"
#include "sample_ring.h"

namespace O3DS
{
	//! Version 0. Predict() returns the last observed sample verbatim
	//! (with t updated to the requested time) - today's "freeze on loss"
	//! behavior, and the universal fallback every other predictor degrades
	//! to when it lacks enough history. Needs 1 sample; Predict() returns
	//! false before the first Observe().
	class HoldPredictor : public IPosePredictor
	{
	public:
		uint32_t Version() const override { return 0; }
		void Observe(const PoseSample& sample) override;
		bool Predict(double t, PoseSample& out) const override;
		void Reset() override;

	private:
		SampleRing<1> mHistory;
	};
}

#endif
