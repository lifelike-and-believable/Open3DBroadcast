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

#include "motion_corpus.h"

#include <cmath>
#include <cstdio>

#include "o3ds/predict/quat_math.h"

namespace O3DS { namespace Eval {

namespace {

	// Fixed-seed 64-bit LCG (Knuth's MMIX constants). Deliberately not
	// <random>: std::normal_distribution is not specified to produce identical
	// sequences across stdlib implementations, which would make eval numbers
	// non-comparable between a dev machine and CI.
	class Lcg
	{
	public:
		explicit Lcg(uint64_t seed) : mState(seed) {}

		double NextUniform() // [0,1)
		{
			mState = mState * 6364136223846793005ULL + 1442695040888963407ULL;
			// Top 53 bits -> double, avoids the poor low-bit behaviour of LCGs.
			return (double)(mState >> 11) * (1.0 / 9007199254740992.0);
		}

		// Box-Muller. Caches the second variate.
		double NextGaussian()
		{
			if (mHasSpare) { mHasSpare = false; return mSpare; }
			double u1 = NextUniform();
			double u2 = NextUniform();
			if (u1 < 1e-12) u1 = 1e-12; // guard log(0)
			const double mag = std::sqrt(-2.0 * std::log(u1));
			mSpare = mag * std::sin(2.0 * 3.14159265358979323846 * u2);
			mHasSpare = true;
			return mag * std::cos(2.0 * 3.14159265358979323846 * u2);
		}

	private:
		uint64_t mState;
		bool     mHasSpare = false;
		double   mSpare    = 0.0;
	};

	// A triangle wave in [-1,1] with period 1/hz — used for the sharp-turn
	// clips. Unlike a sinusoid its derivative is discontinuous at the peaks,
	// which is exactly the case that punishes constant-velocity extrapolation.
	double Triangle(double t, double hz)
	{
		if (hz <= 0.0) return 0.0;
		const double phase = t * hz - std::floor(t * hz); // [0,1)
		return phase < 0.5 ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase);
	}

} // namespace

MotionClip GenerateClip(const ClipSpec& spec)
{
	MotionClip clip;
	clip.name        = spec.name;
	clip.description = spec.description;
	clip.fps         = spec.fps;

	// Seed from the clip name so each clip has its own reproducible noise
	// stream, but the same name always regenerates identically.
	uint64_t seed = 0x9E3779B97F4A7C15ULL;
	for (char c : spec.name) seed = seed * 1099511628211ULL ^ (uint64_t)(unsigned char)c;
	Lcg rng(seed);

	const int frames = (int)(spec.durationS * spec.fps + 0.5);
	clip.samples.reserve((size_t)frames);

	const double dt      = 1.0 / spec.fps;
	const double degToRad = 3.14159265358979323846 / 180.0;

	for (int f = 0; f < frames; ++f)
	{
		const double t = f * dt;

		PoseSample s;
		s.t   = t;
		s.seq = (uint64_t)(f + 1);
		s.translations.reserve((size_t)spec.nodes);
		s.rotations.reserve((size_t)spec.nodes);
		s.scales.reserve((size_t)spec.nodes);
		s.curves.reserve((size_t)spec.curves);

		for (int n = 0; n < spec.nodes; ++n)
		{
			// Per-node phase offset and amplitude scaling: distal joints (higher
			// n) move further and faster than the root, as in a real skeleton.
			const double phase     = (double)n * 0.37;
			const double limbScale = 0.3 + 1.4 * ((double)n / (double)spec.nodes);

			double wave;
			if (spec.sharpTurns)
				wave = Triangle(t + phase * 0.1, spec.gaitHz);
			else
				wave = std::sin(2.0 * 3.14159265358979323846 * spec.gaitHz * t + phase);

			// A slow secondary component so motion is not a single pure tone —
			// real takes always carry drift/breathing under the gait.
			const double drift = 0.15 * std::sin(2.0 * 3.14159265358979323846 * 0.2 * t + phase * 0.5);

			const double a = spec.amplitude * limbScale;
			double x = a * wave + a * drift;
			double y = a * 0.6 * (spec.sharpTurns ? Triangle(t + 0.25 / (spec.gaitHz > 0 ? spec.gaitHz : 1.0), spec.gaitHz)
			                                      : std::cos(2.0 * 3.14159265358979323846 * spec.gaitHz * t + phase));
			double z = a * 0.25 * drift;

			if (spec.noiseSigma > 0.0)
			{
				x += rng.NextGaussian() * spec.noiseSigma;
				y += rng.NextGaussian() * spec.noiseSigma;
				z += rng.NextGaussian() * spec.noiseSigma;
			}
			s.translations.push_back(Vector3d(x, y, z));

			// Rotation about a per-node fixed axis, driven by the same wave.
			double angleDeg = spec.rotAmpDeg * limbScale * wave;
			if (spec.rotNoiseDeg > 0.0)
				angleDeg += rng.NextGaussian() * spec.rotNoiseDeg;

			Vector3d axis(std::sin(phase), std::cos(phase), 0.35);
			const double axisLen = std::sqrt(axis.v[0]*axis.v[0] + axis.v[1]*axis.v[1] + axis.v[2]*axis.v[2]);
			axis = Vector3d(axis.v[0]/axisLen, axis.v[1]/axisLen, axis.v[2]/axisLen);
			s.rotations.push_back(QuatFromAxisAngle(axis, angleDeg * degToRad));

			s.scales.push_back(Vector3d(1.0, 1.0, 1.0));
		}

		for (int c = 0; c < spec.curves; ++c)
		{
			const double cp = (double)c * 0.9;
			double v = 0.5 + 0.5 * std::sin(2.0 * 3.14159265358979323846 * spec.gaitHz * t + cp);
			if (spec.noiseSigma > 0.0)
				v += rng.NextGaussian() * spec.noiseSigma * 0.02;
			s.curves.push_back((float)v);
		}

		clip.samples.push_back(std::move(s));
	}

	return clip;
}

std::vector<ClipSpec> StandardSuite()
{
	std::vector<ClipSpec> suite;

	// Amplitudes are in scene units. The plugin's own docs treat these as
	// centimetres (Unreal convention), so "5" ~ 5 cm of travel.
	{
		ClipSpec s;
		s.name        = "idle";
		s.description = "Standing still: breathing + micro-drift, sensor noise dominates";
		s.gaitHz      = 0.25;
		s.amplitude   = 0.4;
		s.rotAmpDeg   = 1.5;
		s.noiseSigma  = 0.05;
		s.rotNoiseDeg = 0.08;
		suite.push_back(s);
	}
	{
		ClipSpec s;
		s.name        = "walk";
		s.description = "Locomotion at a ~1 Hz gait, light sensor noise";
		s.gaitHz      = 1.0;
		s.amplitude   = 6.0;
		s.rotAmpDeg   = 20.0;
		s.noiseSigma  = 0.05;
		s.rotNoiseDeg = 0.08;
		suite.push_back(s);
	}
	{
		ClipSpec s;
		s.name        = "run";
		s.description = "Fast locomotion at a ~2.5 Hz gait";
		s.gaitHz      = 2.5;
		s.amplitude   = 15.0;
		s.rotAmpDeg   = 45.0;
		s.noiseSigma  = 0.05;
		s.rotNoiseDeg = 0.08;
		suite.push_back(s);
	}
	{
		ClipSpec s;
		s.name        = "sharp";
		s.description = "Abrupt direction reversals (triangle wave) - worst case for extrapolation";
		s.gaitHz      = 1.5;
		s.amplitude   = 12.0;
		s.rotAmpDeg   = 35.0;
		s.noiseSigma  = 0.05;
		s.rotNoiseDeg = 0.08;
		s.sharpTurns  = true;
		suite.push_back(s);
	}
	{
		ClipSpec s;
		s.name        = "walk_noisy";
		s.description = "Same gait as 'walk' with 10x sensor noise - tests noise amplification";
		s.gaitHz      = 1.0;
		s.amplitude   = 6.0;
		s.rotAmpDeg   = 20.0;
		s.noiseSigma  = 0.5;
		s.rotNoiseDeg = 0.8;
		suite.push_back(s);
	}

	return suite;
}

std::vector<ClipSpec> NoiseSweep()
{
	// Fixed gait, fixed amplitude; only the sensor noise varies. Holding
	// everything else constant is what makes the crossover attributable to
	// SNR rather than to the motion itself.
	static const double kSigmas[] = { 0.0, 0.01, 0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 1.0 };

	std::vector<ClipSpec> sweep;
	for (double sigma : kSigmas)
	{
		ClipSpec s;
		char buf[64];
		std::snprintf(buf, sizeof(buf), "noise_%.3f", sigma);
		s.name        = buf;
		std::snprintf(buf, sizeof(buf), "walk gait, sensor sigma = %.3f units", sigma);
		s.description = buf;
		s.gaitHz      = 1.0;
		s.amplitude   = 6.0;
		s.rotAmpDeg   = 20.0;
		s.noiseSigma  = sigma;
		s.rotNoiseDeg = sigma * 1.6; // keep rotational noise proportional
		sweep.push_back(s);
	}
	return sweep;
}

}} // namespace O3DS::Eval
