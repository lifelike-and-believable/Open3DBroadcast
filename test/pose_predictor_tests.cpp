// Acceptance matrix for src/o3ds/predict/*, per the roadmap doc's C0 spec
// (docs/roadmap/resilient-streaming-and-motion-prediction.md, §5/C0).
#include "test_framework.h"

#include "o3ds/predict/quat_math.h"
#include "o3ds/predict/hold_predictor.h"
#include "o3ds/predict/linear_predictor.h"
#include "o3ds/predict/quadratic_predictor.h"
#include "o3ds/predict/sample_ring.h"

#include <cmath>

using namespace O3DS;

namespace
{
	constexpr double kPi = 3.14159265358979323846;

	double QuatDot(const Quat& a, const Quat& b)
	{
		return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3];
	}

	// |1 - |q|^2| tolerance check that q is (numerically) a unit quaternion.
	bool IsUnitNorm(const Quat& q, double tol = 1.0e-9)
	{
		const double lenSq = q.v[0] * q.v[0] + q.v[1] * q.v[1] + q.v[2] * q.v[2] + q.v[3] * q.v[3];
		return std::abs(lenSq - 1.0) < tol;
	}

	// Quaternions q and -q represent the same rotation; compare via |dot|.
	bool QuatsNearlyEqual(const Quat& a, const Quat& b, double tol = 1.0e-6)
	{
		return std::abs(std::abs(QuatDot(a, b)) - 1.0) < tol;
	}

	PoseSample MakeSample(double t, uint64_t seq, double x, double y, double z, const Quat& rot, float curve)
	{
		PoseSample s;
		s.t = t;
		s.seq = seq;
		s.translations.push_back(Vector3d(x, y, z));
		s.rotations.push_back(rot);
		s.scales.push_back(Vector3d(1.0, 1.0, 1.0));
		s.curves.push_back(curve);
		return s;
	}
}

// ---------------------------------------------------------------------------
// quat_math
// ---------------------------------------------------------------------------

O3DS_TEST(QuatMath_Multiply_WithIdentity_ReturnsOperand)
{
	Quat q = QuatFromAxisAngle(Vector3d(0.0, 1.0, 0.0), kPi / 3.0);
	Quat r1 = QuatMultiply(QuatIdentity(), q);
	Quat r2 = QuatMultiply(q, QuatIdentity());

	O3DS_CHECK(QuatsNearlyEqual(r1, q));
	O3DS_CHECK(QuatsNearlyEqual(r2, q));
}

O3DS_TEST(QuatMath_ConjugateOfUnitQuat_IsItsInverse)
{
	Quat q = QuatFromAxisAngle(Vector3d(1.0, 2.0, 3.0), 1.234);
	Quat inv = QuatConjugate(q);
	Quat product = QuatMultiply(q, inv);

	O3DS_CHECK(QuatsNearlyEqual(product, QuatIdentity()));
}

O3DS_TEST(QuatMath_AxisAngleRoundTrip_RecoversRotation)
{
	Vector3d axis(0.267261, 0.534522, 0.801784); // normalized (1,2,3)
	double angle = 0.789;

	Quat q = QuatFromAxisAngle(axis, angle);
	O3DS_CHECK(IsUnitNorm(q));

	Vector3d outAxis;
	double outAngle = 0.0;
	O3DS_CHECK(QuatToAxisAngle(q, outAxis, outAngle));

	Quat rebuilt = QuatFromAxisAngle(outAxis, outAngle);
	O3DS_CHECK(QuatsNearlyEqual(q, rebuilt));
}

O3DS_TEST(QuatMath_ToAxisAngle_NearIdentity_ReturnsFalse)
{
	Vector3d axis;
	double angle = 0.0;
	O3DS_CHECK(!QuatToAxisAngle(QuatIdentity(), axis, angle));
}

O3DS_TEST(QuatMath_Normalize_ScaledQuat_RecoversUnitNorm)
{
	Quat q(2.0, 0.0, 0.0, 2.0); // unnormalized, direction (1,0,0,1)
	Quat n = QuatNormalize(q);
	O3DS_CHECK(IsUnitNorm(n));
}

// ---------------------------------------------------------------------------
// SampleRing
// ---------------------------------------------------------------------------

O3DS_TEST(SampleRing_PushBeyondCapacity_KeepsNewestNInOrder)
{
	SampleRing<3> ring;
	for (int i = 0; i < 5; ++i)
	{
		ring.Push(MakeSample((double)i, (uint64_t)i, (double)i, 0, 0, QuatIdentity(), 0.0f));
	}

	O3DS_CHECK_EQ(ring.Count(), (size_t)3);
	O3DS_CHECK_EQ(ring[0].seq, (uint64_t)2);
	O3DS_CHECK_EQ(ring[1].seq, (uint64_t)3);
	O3DS_CHECK_EQ(ring[2].seq, (uint64_t)4);
}

O3DS_TEST(SampleRing_Clear_ResetsCount)
{
	SampleRing<2> ring;
	ring.Push(MakeSample(0.0, 1, 0, 0, 0, QuatIdentity(), 0.0f));
	ring.Clear();
	O3DS_CHECK_EQ(ring.Count(), (size_t)0);
}

// ---------------------------------------------------------------------------
// HoldPredictor
// ---------------------------------------------------------------------------

O3DS_TEST(HoldPredictor_NoObservations_PredictReturnsFalse)
{
	HoldPredictor pred;
	PoseSample out;
	O3DS_CHECK(!pred.Predict(1.0, out));
}

O3DS_TEST(HoldPredictor_OneObservation_HoldsLastValueAtRequestedTime)
{
	HoldPredictor pred;
	pred.Observe(MakeSample(0.0, 5, 10.0, 20.0, 30.0, QuatIdentity(), 0.5f));

	PoseSample out;
	O3DS_CHECK(pred.Predict(2.5, out));
	O3DS_CHECK_EQ(out.t, 2.5);
	O3DS_CHECK_EQ(out.translations[0].v[0], 10.0);
	O3DS_CHECK_EQ(out.curves[0], 0.5f);
}

O3DS_TEST(HoldPredictor_Reset_ClearsHistory)
{
	HoldPredictor pred;
	pred.Observe(MakeSample(0.0, 1, 1, 1, 1, QuatIdentity(), 0.0f));
	pred.Reset();

	PoseSample out;
	O3DS_CHECK(!pred.Predict(1.0, out));
}

// ---------------------------------------------------------------------------
// LinearPredictor
// ---------------------------------------------------------------------------

O3DS_TEST(LinearPredictor_InsufficientHistory_ReturnsFalse)
{
	LinearPredictor pred;
	PoseSample out;
	O3DS_CHECK(!pred.Predict(1.0, out));

	pred.Observe(MakeSample(0.0, 1, 0, 0, 0, QuatIdentity(), 0.0f));
	O3DS_CHECK(!pred.Predict(1.0, out)); // only 1 sample so far
}

O3DS_TEST(LinearPredictor_ConstantVelocityTranslation_PredictsExactly)
{
	// x(t) = 5 + 2*t
	LinearPredictor pred;
	pred.Observe(MakeSample(0.0, 1, 5.0, 0.0, 0.0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(1.0, 2, 7.0, 0.0, 0.0, QuatIdentity(), 0.0f));

	PoseSample out;
	O3DS_CHECK(pred.Predict(2.0, out));
	O3DS_CHECK(std::abs(out.translations[0].v[0] - 9.0) < 1.0e-9);
}

O3DS_TEST(LinearPredictor_ConstantAngularVelocity_PredictsExactlyAndStaysUnitNorm)
{
	// Constant rotation about Z at 0.4 rad per unit time.
	Vector3d axisZ(0.0, 0.0, 1.0);
	Quat q0 = QuatFromAxisAngle(axisZ, 0.0);
	Quat q1 = QuatFromAxisAngle(axisZ, 0.4);
	Quat qExpectedAt2 = QuatFromAxisAngle(axisZ, 0.8);

	LinearPredictor pred;
	pred.Observe(MakeSample(0.0, 1, 0, 0, 0, q0, 0.0f));
	pred.Observe(MakeSample(1.0, 2, 0, 0, 0, q1, 0.0f));

	PoseSample out;
	O3DS_CHECK(pred.Predict(2.0, out));
	O3DS_CHECK(IsUnitNorm(out.rotations[0]));
	O3DS_CHECK(QuatsNearlyEqual(out.rotations[0], qExpectedAt2, 1.0e-6));
}

O3DS_TEST(LinearPredictor_ErrorMuchLessThanHold_AtOneFrameHorizon)
{
	// Ground truth: constant-velocity motion x(t) = 3*t. Compare LinearPredictor
	// vs HoldPredictor error at a one-frame-ahead horizon (roadmap acceptance:
	// "LinearPredictor error << HoldPredictor at horizon = 1 frame").
	const double frameDt = 1.0 / 60.0;

	LinearPredictor linear;
	HoldPredictor hold;

	double t0 = 0.0, t1 = frameDt;
	double x0 = 3.0 * t0, x1 = 3.0 * t1;

	linear.Observe(MakeSample(t0, 1, x0, 0, 0, QuatIdentity(), 0.0f));
	linear.Observe(MakeSample(t1, 2, x1, 0, 0, QuatIdentity(), 0.0f));
	hold.Observe(MakeSample(t1, 2, x1, 0, 0, QuatIdentity(), 0.0f));

	double tHorizon = t1 + frameDt;
	double trueX = 3.0 * tHorizon;

	PoseSample linearOut, holdOut;
	O3DS_CHECK(linear.Predict(tHorizon, linearOut));
	O3DS_CHECK(hold.Predict(tHorizon, holdOut));

	double linearError = std::abs(linearOut.translations[0].v[0] - trueX);
	double holdError = std::abs(holdOut.translations[0].v[0] - trueX);

	O3DS_CHECK(holdError > 1.0e-6); // sanity: hold actually has real error here
	O3DS_CHECK(linearError < holdError * 0.01);
}

O3DS_TEST(LinearPredictor_DegenerateSpacing_HoldsRatherThanDividesByZero)
{
	LinearPredictor pred;
	pred.Observe(MakeSample(1.0, 1, 5.0, 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(1.0, 2, 7.0, 0, 0, QuatIdentity(), 0.0f)); // same t as previous

	PoseSample out;
	O3DS_CHECK(pred.Predict(2.0, out));
	O3DS_CHECK_EQ(out.translations[0].v[0], 7.0); // held, not NaN/inf
}

O3DS_TEST(LinearPredictor_Reset_ClearsHistory)
{
	LinearPredictor pred;
	pred.Observe(MakeSample(0.0, 1, 0, 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(1.0, 2, 1, 0, 0, QuatIdentity(), 0.0f));
	pred.Reset();

	PoseSample out;
	O3DS_CHECK(!pred.Predict(2.0, out));
}

// ---------------------------------------------------------------------------
// QuadraticPredictor
// ---------------------------------------------------------------------------

O3DS_TEST(QuadraticPredictor_InsufficientHistory_ReturnsFalse)
{
	QuadraticPredictor pred;
	PoseSample out;
	O3DS_CHECK(!pred.Predict(1.0, out));

	pred.Observe(MakeSample(0.0, 1, 0, 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(1.0, 2, 1, 0, 0, QuatIdentity(), 0.0f));
	O3DS_CHECK(!pred.Predict(2.0, out)); // only 2 samples so far
}

O3DS_TEST(QuadraticPredictor_ConstantAccelerationTranslation_PredictsExactly)
{
	// x(t) = 1 + 2*t + 0.5*4*t^2 = 1 + 2t + 2t^2. Three points exactly
	// determine a quadratic, so extrapolation should be exact (to fp
	// precision) regardless of spacing.
	auto x = [](double t) { return 1.0 + 2.0 * t + 2.0 * t * t; };

	QuadraticPredictor pred;
	pred.Observe(MakeSample(0.0, 1, x(0.0), 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(0.3, 2, x(0.3), 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(0.7, 3, x(0.7), 0, 0, QuatIdentity(), 0.0f));

	PoseSample out;
	O3DS_CHECK(pred.Predict(1.5, out));
	O3DS_CHECK(std::abs(out.translations[0].v[0] - x(1.5)) < 1.0e-9);
}

O3DS_TEST(QuadraticPredictor_RotationStaysUnitNorm)
{
	Vector3d axisY(0.0, 1.0, 0.0);
	QuadraticPredictor pred;
	pred.Observe(MakeSample(0.0, 1, 0, 0, 0, QuatFromAxisAngle(axisY, 0.0), 0.0f));
	pred.Observe(MakeSample(0.5, 2, 0, 0, 0, QuatFromAxisAngle(axisY, 0.2), 0.0f));
	pred.Observe(MakeSample(1.0, 3, 0, 0, 0, QuatFromAxisAngle(axisY, 0.5), 0.0f));

	PoseSample out;
	O3DS_CHECK(pred.Predict(1.5, out));
	O3DS_CHECK(IsUnitNorm(out.rotations[0]));
}

O3DS_TEST(QuadraticPredictor_DegenerateSpacing_HoldsRatherThanDividesByZero)
{
	QuadraticPredictor pred;
	pred.Observe(MakeSample(0.0, 1, 1.0, 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(1.0, 2, 2.0, 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(1.0, 3, 3.0, 0, 0, QuatIdentity(), 0.0f)); // same t as previous

	PoseSample out;
	O3DS_CHECK(pred.Predict(2.0, out));
	O3DS_CHECK_EQ(out.translations[0].v[0], 3.0); // held, not NaN/inf
}

O3DS_TEST(QuadraticPredictor_Reset_ClearsHistory)
{
	QuadraticPredictor pred;
	pred.Observe(MakeSample(0.0, 1, 0, 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(1.0, 2, 1, 0, 0, QuatIdentity(), 0.0f));
	pred.Observe(MakeSample(2.0, 3, 2, 0, 0, QuatIdentity(), 0.0f));
	pred.Reset();

	PoseSample out;
	O3DS_CHECK(!pred.Predict(3.0, out));
}

O3DS_TEST(Predictors_VersionTags_MatchRoadmapNumbering)
{
	HoldPredictor hold;
	LinearPredictor linear;
	QuadraticPredictor quadratic;

	O3DS_CHECK_EQ(hold.Version(), (uint32_t)0);
	O3DS_CHECK_EQ(linear.Version(), (uint32_t)1);
	O3DS_CHECK_EQ(quadratic.Version(), (uint32_t)2);
}
