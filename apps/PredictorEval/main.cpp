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

// PredictorEval — measures how well the C0 classical baselines predict a pose
// N frames ahead, which is the evidence the roadmap's C3 go/no-go depends on:
//
//   "Treat C3 as a spike with a go/no-go gated on the prediction-error numbers
//    from C1/C2, not a committed deliverable."
//   — docs/roadmap/resilient-streaming-and-motion-prediction.md, §5/C3
//
// See README.md next to this file for what the numbers do and do not support.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "motion_corpus.h"

#include "o3ds/predict/hold_predictor.h"
#include "o3ds/predict/linear_predictor.h"
#include "o3ds/predict/quadratic_predictor.h"
#include "o3ds/predict/quat_math.h"

using namespace O3DS;
using namespace O3DS::Eval;

namespace {

struct ErrorStats
{
	double transRmse   = 0.0; //!< scene units
	double transP95    = 0.0; //!< scene units
	double rotMeanDeg  = 0.0;
	double rotP95Deg   = 0.0;
	long   predictions = 0;   //!< scored predictions
	long   declined    = 0;   //!< Predict() returned false (insufficient history)
};

double Percentile(std::vector<double>& v, double p)
{
	if (v.empty()) return 0.0;
	std::sort(v.begin(), v.end());
	const double idx = p * (double)(v.size() - 1);
	const size_t lo  = (size_t)std::floor(idx);
	const size_t hi  = (size_t)std::ceil(idx);
	if (lo == hi) return v[lo];
	const double frac = idx - (double)lo;
	return v[lo] * (1.0 - frac) + v[hi] * frac;
}

//! Geodesic angle between two unit quaternions, in degrees. q and -q are the
//! same rotation, so the shortest arc is taken via |dot|.
double QuatAngleDeg(const Quat& a, const Quat& b)
{
	double dot = a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2] + a.v[3]*b.v[3];
	dot = std::fabs(dot);
	if (dot > 1.0) dot = 1.0;
	return 2.0 * std::acos(dot) * 180.0 / 3.14159265358979323846;
}

//! Score one predictor over one clip at a fixed prediction horizon.
//!
//! Protocol: walk the clip in order. At each frame i, the predictor has
//! Observe()d frames [0..i]. We then ask it for the pose at frame i+horizon
//! and compare against that frame as actually captured.
//!
//! The predictor observes the *transmitted* (noisy) samples, and is scored
//! against the *transmitted* sample it was asked to predict — not against a
//! hypothetical clean signal. That is the operationally correct target for
//! concealment (C1): when a frame is lost, the receiver's job is to reproduce
//! the frame that would have been displayed, noise and all.
ErrorStats Evaluate(IPosePredictor& pred, const MotionClip& clip, int horizon)
{
	ErrorStats st;
	pred.Reset();

	std::vector<double> transErrs;
	std::vector<double> rotErrs;
	double transSqSum = 0.0;
	double rotSum     = 0.0;

	const size_t n = clip.samples.size();
	for (size_t i = 0; i + (size_t)horizon < n; ++i)
	{
		pred.Observe(clip.samples[i]);

		const PoseSample& truth = clip.samples[i + (size_t)horizon];

		PoseSample out;
		if (!pred.Predict(truth.t, out))
		{
			++st.declined;
			continue;
		}

		// A predictor that returns true must fill every channel it was given.
		// If it doesn't, that's a bug worth surfacing loudly rather than
		// silently scoring a shorter vector.
		if (out.translations.size() != truth.translations.size() ||
		    out.rotations.size()    != truth.rotations.size())
		{
			std::fprintf(stderr,
				"ERROR: predictor returned %zu translations / %zu rotations, "
				"expected %zu / %zu\n",
				out.translations.size(), out.rotations.size(),
				truth.translations.size(), truth.rotations.size());
			std::exit(2);
		}

		for (size_t k = 0; k < truth.translations.size(); ++k)
		{
			const double d = dist(out.translations[k], truth.translations[k]);
			transErrs.push_back(d);
			transSqSum += d * d;
		}
		for (size_t k = 0; k < truth.rotations.size(); ++k)
		{
			const double d = QuatAngleDeg(out.rotations[k], truth.rotations[k]);
			rotErrs.push_back(d);
			rotSum += d;
		}
		++st.predictions;
	}

	if (!transErrs.empty())
	{
		st.transRmse = std::sqrt(transSqSum / (double)transErrs.size());
		st.transP95  = Percentile(transErrs, 0.95);
	}
	if (!rotErrs.empty())
	{
		st.rotMeanDeg = rotSum / (double)rotErrs.size();
		st.rotP95Deg  = Percentile(rotErrs, 0.95);
	}
	return st;
}

struct NamedPredictor
{
	const char*                     name;
	std::unique_ptr<IPosePredictor> pred;
};

std::vector<NamedPredictor> MakePredictors()
{
	std::vector<NamedPredictor> v;
	v.push_back({ "Hold",      std::unique_ptr<IPosePredictor>(new HoldPredictor()) });
	v.push_back({ "Linear",    std::unique_ptr<IPosePredictor>(new LinearPredictor()) });
	v.push_back({ "Quadratic", std::unique_ptr<IPosePredictor>(new QuadraticPredictor()) });
	return v;
}

void PrintUsage(const char* argv0)
{
	std::printf(
		"Usage: %s [--horizons 1,2,3,5] [--csv]\n"
		"\n"
		"Measures prediction error for the C0 classical baselines over a\n"
		"synthetic motion suite, to inform the roadmap's C3 go/no-go.\n"
		"\n"
		"  --horizons N[,N...]  Prediction horizons in frames (default 1,2,3,5)\n"
		"  --noise-sweep        Sweep sensor noise at a fixed gait to locate the\n"
		"                       SNR where extrapolation stops beating Hold\n"
		"  --csv                Emit machine-readable CSV instead of a table\n"
		"  --help               This message\n",
		argv0);
}

} // namespace

int main(int argc, char** argv)
{
	std::vector<int> horizons = { 1, 2, 3, 5 };
	bool csv = false;
	bool noiseSweep = false;

	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--help") == 0) { PrintUsage(argv[0]); return 0; }
		else if (std::strcmp(argv[i], "--csv") == 0) { csv = true; }
		else if (std::strcmp(argv[i], "--noise-sweep") == 0) { noiseSweep = true; }
		else if (std::strcmp(argv[i], "--horizons") == 0 && i + 1 < argc)
		{
			horizons.clear();
			const std::string arg = argv[++i];
			size_t pos = 0;
			while (pos <= arg.size())
			{
				const size_t comma = arg.find(',', pos);
				const std::string tok = arg.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
				if (!tok.empty()) horizons.push_back(std::atoi(tok.c_str()));
				if (comma == std::string::npos) break;
				pos = comma + 1;
			}
			if (horizons.empty()) { std::fprintf(stderr, "--horizons: no valid values\n"); return 1; }
		}
		else { std::fprintf(stderr, "Unknown argument: %s\n", argv[i]); PrintUsage(argv[0]); return 1; }
	}

	const std::vector<ClipSpec> suite = noiseSweep ? NoiseSweep() : StandardSuite();

	if (csv)
		std::printf("clip,horizon_frames,horizon_ms,predictor,trans_rmse,trans_p95,rot_mean_deg,rot_p95_deg,predictions,declined\n");
	else
	{
		std::printf("PredictorEval - C0 baseline prediction error\n");
		std::printf("=============================================\n\n");
		std::printf("SYNTHETIC motion suite (no .o3dscap corpus exists in this repo).\n");
		std::printf("Compare predictors against each other; do not read absolute\n");
		std::printf("magnitudes as real-mocap error. See README.md.\n\n");
		std::printf("Translation errors are in scene units (~cm). Rotation in degrees.\n");
		if (noiseSweep)
			std::printf("\nNOISE SWEEP: gait held constant, only sensor sigma varies.\n");
	}

	for (const ClipSpec& spec : suite)
	{
		const MotionClip clip = GenerateClip(spec);

		if (!csv)
		{
			std::printf("\n%s - %s\n", clip.name.c_str(), clip.description.c_str());
			std::printf("  %d frames @ %.0f fps\n", (int)clip.samples.size(), clip.fps);
		}

		for (int h : horizons)
		{
			const double horizonMs = 1000.0 * (double)h / clip.fps;

			if (!csv)
			{
				std::printf("\n  horizon %d frame%s (%.1f ms)\n", h, h == 1 ? "" : "s", horizonMs);
				std::printf("    %-10s  %10s  %10s  %10s  %10s\n",
					"predictor", "trans RMSE", "trans P95", "rot mean", "rot P95");
			}

			// Fresh predictors per (clip, horizon) so no history leaks across runs.
			std::vector<NamedPredictor> preds = MakePredictors();

			double holdRmse = 0.0;
			for (NamedPredictor& np : preds)
			{
				const ErrorStats st = Evaluate(*np.pred, clip, h);
				if (std::strcmp(np.name, "Hold") == 0) holdRmse = st.transRmse;

				if (csv)
				{
					std::printf("%s,%d,%.2f,%s,%.6f,%.6f,%.6f,%.6f,%ld,%ld\n",
						clip.name.c_str(), h, horizonMs, np.name,
						st.transRmse, st.transP95, st.rotMeanDeg, st.rotP95Deg,
						st.predictions, st.declined);
				}
				else
				{
					// Relative-to-Hold column is the number the C3 gate turns on:
					// if extrapolation isn't beating "just hold the last pose",
					// a learned model has to clear a much higher bar to be worth it.
					char rel[32] = "  (baseline)";
					if (std::strcmp(np.name, "Hold") != 0 && holdRmse > 0.0)
					{
						const double pct = 100.0 * (st.transRmse - holdRmse) / holdRmse;
						std::snprintf(rel, sizeof(rel), "  %+.1f%% vs Hold", pct);
					}
					std::printf("    %-10s  %10.4f  %10.4f  %10.4f  %10.4f%s\n",
						np.name, st.transRmse, st.transP95, st.rotMeanDeg, st.rotP95Deg, rel);
				}
			}
		}
	}

	if (!csv) std::printf("\n");
	return 0;
}
