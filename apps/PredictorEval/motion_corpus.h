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

#ifndef O3DS_PREDICTOREVAL_MOTION_CORPUS_H
#define O3DS_PREDICTOREVAL_MOTION_CORPUS_H

#include <cstdint>
#include <string>
#include <vector>

#include "o3ds/predict/pose_predictor.h"

namespace O3DS { namespace Eval {

	//! One named synthetic motion clip: a dense, uniformly-sampled sequence
	//! of PoseSamples standing in for a real mocap take.
	//!
	//! ⚠️ These are a PROXY, not real mocap. They exist because the repo has
	//! no .o3dscap corpus (see README.md next to this file). Their value is
	//! comparative — how the baselines rank against each other as motion gets
	//! faster, sharper, and noisier — not absolute error magnitudes.
	struct MotionClip
	{
		std::string           name;
		std::string           description;
		double                fps = 60.0;
		std::vector<PoseSample> samples; //!< what the sender captured & sent
	};

	//! Deterministic generator parameters for one clip.
	struct ClipSpec
	{
		std::string name;
		std::string description;
		double gaitHz     = 0.0;  //!< dominant periodic frequency (0 = none)
		double amplitude  = 0.0;  //!< translation amplitude, scene units
		double rotAmpDeg  = 0.0;  //!< rotation amplitude, degrees
		double noiseSigma = 0.0;  //!< per-sample Gaussian sensor noise, scene units
		double rotNoiseDeg = 0.0; //!< per-sample Gaussian rotation noise, degrees
		bool   sharpTurns = false;//!< inject abrupt direction reversals
		double durationS  = 10.0;
		double fps        = 60.0;
		int    nodes      = 20;
		int    curves     = 8;
	};

	//! Build one clip from its spec. Fully deterministic: the same spec always
	//! produces byte-identical samples (fixed-seed LCG, no <random> engine
	//! variation across stdlib implementations), so eval runs are reproducible
	//! and comparable across machines and CI.
	MotionClip GenerateClip(const ClipSpec& spec);

	//! The standard suite used by the C3 gate report. Ordered from easiest to
	//! hardest for an extrapolating predictor.
	std::vector<ClipSpec> StandardSuite();

	//! A sweep over sensor-noise level at a fixed gait, used to locate the
	//! SNR at which extrapolation stops beating "hold the last pose". This is
	//! the actionable number: it's the threshold a runtime predictor-selection
	//! heuristic would switch on.
	std::vector<ClipSpec> NoiseSweep();

}} // namespace O3DS::Eval

#endif
