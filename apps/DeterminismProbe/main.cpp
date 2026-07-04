// C2 prerequisite (roadmap doc §6/D... see §5 Phase C2's "Float determinism"
// risk note): before committing to a chained predictor for residual coding,
// the roadmap calls for prototyping and measuring drift on the target
// platforms. This tool runs a long, deterministic synthetic capture through
// a chained LinearPredictor/QuadraticPredictor exactly the way C2's sender
// and receiver would (Predict() before Observe() every frame, accumulating
// the residual), then prints the aggregate result.
//
// Run it once built with GCC (Linux CI) and once with MSVC (the self-hosted
// UE Windows runner) and diff the two outputs. Textually identical output
// means no measurable cross-compiler float drift over this run length;
// any difference is the drift signal the roadmap asks for. The synthetic
// corpus itself is generated from std::mt19937_64 (specified bit-for-bit by
// the C++ standard) so any divergence in the two runs' output comes from the
// predictor/quaternion math, not from the input data differing.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>
#include <memory>

#include "o3ds/predict/pose_predictor.h"
#include "o3ds/predict/linear_predictor.h"
#include "o3ds/predict/quadratic_predictor.h"
#include "o3ds/predict/quat_math.h"
#include "o3ds/o3ds_version.h"

using namespace O3DS;

namespace
{
	constexpr int kNumJoints = 24;
	constexpr int kNumCurves = 8;
	constexpr int kNumFrames = 20000; // ~333s at 60fps - far beyond any realistic keyframe cadence
	constexpr double kFrameDt = 1.0 / 60.0;
	constexpr uint64_t kSeed = 0x4F334453'43323031ull; // "O3DS" + "C201", arbitrary but fixed

	// Same construction as channel_model.cpp's own NextUniform01 - see that
	// file's comment on why this form (not std::uniform_real_distribution,
	// whose exact output is unspecified across standard library
	// implementations) is used for a value that must be bit-identical
	// across platforms.
	double NextUniform01(std::mt19937_64& rng)
	{
		const double kTwoPow64 = 18446744073709551616.0;
		return (double)rng() / kTwoPow64;
	}

	double NextInRange(std::mt19937_64& rng, double lo, double hi)
	{
		return lo + NextUniform01(rng) * (hi - lo);
	}

	struct JointMotion
	{
		Vector3d basePos;
		Vector3d velocity;
		Vector3d accel; // slight curvature so QuadraticPredictor isn't a perfect fit either
		Vector3d axis;
		double angularVel = 0.0;
		double angularAccel = 0.0;
	};

	std::vector<JointMotion> MakeJoints(std::mt19937_64& rng)
	{
		std::vector<JointMotion> joints(kNumJoints);
		for (JointMotion& j : joints)
		{
			j.basePos = Vector3d(NextInRange(rng, -100.0, 100.0), NextInRange(rng, -100.0, 100.0), NextInRange(rng, -100.0, 100.0));
			j.velocity = Vector3d(NextInRange(rng, -5.0, 5.0), NextInRange(rng, -5.0, 5.0), NextInRange(rng, -5.0, 5.0));
			j.accel = Vector3d(NextInRange(rng, -0.5, 0.5), NextInRange(rng, -0.5, 0.5), NextInRange(rng, -0.5, 0.5));
			j.axis = Vector3d(NextInRange(rng, -1.0, 1.0), NextInRange(rng, -1.0, 1.0), NextInRange(rng, -1.0, 1.0));
			j.angularVel = NextInRange(rng, -2.0, 2.0);
			j.angularAccel = NextInRange(rng, -0.2, 0.2);
		}
		return joints;
	}

	std::vector<double> MakeCurveFreqs(std::mt19937_64& rng)
	{
		std::vector<double> freqs(kNumCurves);
		for (double& f : freqs) f = NextInRange(rng, 0.2, 3.0);
		return freqs;
	}

	PoseSample BuildFrame(double t, uint64_t seq, const std::vector<JointMotion>& joints, const std::vector<double>& curveFreqs)
	{
		PoseSample s;
		s.t = t;
		s.seq = seq;
		s.translations.resize(joints.size());
		s.rotations.resize(joints.size());
		s.scales.assign(joints.size(), Vector3d(1.0, 1.0, 1.0));
		s.curves.resize(curveFreqs.size());

		for (size_t i = 0; i < joints.size(); ++i)
		{
			const JointMotion& j = joints[i];
			for (int c = 0; c < 3; ++c)
			{
				s.translations[i].v[c] = j.basePos.v[c] + j.velocity.v[c] * t + 0.5 * j.accel.v[c] * t * t;
			}
			const double angle = j.angularVel * t + 0.5 * j.angularAccel * t * t;
			s.rotations[i] = QuatFromAxisAngle(j.axis, angle);
		}

		for (size_t i = 0; i < curveFreqs.size(); ++i)
		{
			s.curves[i] = (float)(std::sin(curveFreqs[i] * t) * 0.5 + 0.5);
		}

		return s;
	}

	// Folds every double touched into a single running fingerprint (FNV-1a
	// over the raw bit pattern) so two runs are "identical" iff this value
	// matches - a cheap, exact stand-in for diffing every printed digit.
	struct Fingerprint
	{
		uint64_t value = 1469598103934665603ull; // FNV-1a offset basis

		void Fold(double v)
		{
			uint64_t bits;
			static_assert(sizeof(bits) == sizeof(v), "double must be 64-bit");
			std::memcpy(&bits, &v, sizeof(bits));
			value ^= bits;
			value *= 1099511628211ull; // FNV-1a prime
		}

		void Fold(const Vector3d& v) { Fold(v.v[0]); Fold(v.v[1]); Fold(v.v[2]); }
		void Fold(const Quat& q) { Fold(q.v[0]); Fold(q.v[1]); Fold(q.v[2]); Fold(q.v[3]); }
	};

	struct ResidualStats
	{
		double translationAbsSum = 0.0;
		double translationAbsMax = 0.0;
		double rotationAngleSum = 0.0; // radians, magnitude of the predicted->actual delta rotation
		double rotationAngleMax = 0.0;
		double curveAbsSum = 0.0;
		uint64_t predictedFrameCount = 0;
		Fingerprint fp;
	};

	void AccumulateResidual(ResidualStats& stats, const PoseSample& actual, const PoseSample& predicted)
	{
		++stats.predictedFrameCount;

		const size_t nTrans = std::min(actual.translations.size(), predicted.translations.size());
		for (size_t i = 0; i < nTrans; ++i)
		{
			double dx = actual.translations[i].v[0] - predicted.translations[i].v[0];
			double dy = actual.translations[i].v[1] - predicted.translations[i].v[1];
			double dz = actual.translations[i].v[2] - predicted.translations[i].v[2];
			double mag = std::sqrt(dx * dx + dy * dy + dz * dz);
			stats.translationAbsSum += mag;
			stats.translationAbsMax = std::max(stats.translationAbsMax, mag);
			stats.fp.Fold(predicted.translations[i]);
		}

		const size_t nRot = std::min(actual.rotations.size(), predicted.rotations.size());
		for (size_t i = 0; i < nRot; ++i)
		{
			const Quat dq = QuatMultiply(actual.rotations[i], QuatConjugate(predicted.rotations[i]));
			Vector3d axis;
			double angle = 0.0;
			if (QuatToAxisAngle(dq, axis, angle))
			{
				stats.rotationAngleSum += angle;
				stats.rotationAngleMax = std::max(stats.rotationAngleMax, angle);
			}
			stats.fp.Fold(predicted.rotations[i]);
		}

		const size_t nCurves = std::min(actual.curves.size(), predicted.curves.size());
		for (size_t i = 0; i < nCurves; ++i)
		{
			stats.curveAbsSum += std::abs((double)actual.curves[i] - (double)predicted.curves[i]);
			stats.fp.Fold((double)predicted.curves[i]);
		}
	}

	void RunPredictor(const char* name, std::unique_ptr<IPosePredictor> predictor, const std::vector<JointMotion>& joints, const std::vector<double>& curveFreqs)
	{
		ResidualStats stats;

		for (int i = 0; i < kNumFrames; ++i)
		{
			const double t = (double)i * kFrameDt;
			const PoseSample frame = BuildFrame(t, (uint64_t)i, joints, curveFreqs);

			PoseSample predicted;
			if (predictor->Predict(t, predicted))
			{
				AccumulateResidual(stats, frame, predicted);
			}

			predictor->Observe(frame);
		}

		printf("== %s (version=%u) ==\n", name, predictor->Version());
		printf("  frames_predicted:        %llu\n", (unsigned long long)stats.predictedFrameCount);
		printf("  translation_abs_sum:     %.17g\n", stats.translationAbsSum);
		printf("  translation_abs_max:     %.17g\n", stats.translationAbsMax);
		printf("  rotation_angle_sum_rad:  %.17g\n", stats.rotationAngleSum);
		printf("  rotation_angle_max_rad:  %.17g\n", stats.rotationAngleMax);
		printf("  curve_abs_sum:           %.17g\n", stats.curveAbsSum);
		printf("  fingerprint:             0x%016llx\n", (unsigned long long)stats.fp.value);
		printf("\n");
	}
}

int main()
{
	printf("O3DS DeterminismProbe (C2 prerequisite) - %s\n", O3DS::getVersion());
	printf("frames=%d dt=%.6f joints=%d curves=%d seed=0x%016llx\n\n", kNumFrames, kFrameDt, kNumJoints, kNumCurves, (unsigned long long)kSeed);

	std::mt19937_64 rng(kSeed);
	const std::vector<JointMotion> joints = MakeJoints(rng);
	const std::vector<double> curveFreqs = MakeCurveFreqs(rng);

	RunPredictor("LinearPredictor", std::make_unique<LinearPredictor>(), joints, curveFreqs);
	RunPredictor("QuadraticPredictor", std::make_unique<QuadraticPredictor>(), joints, curveFreqs);

	return 0;
}
