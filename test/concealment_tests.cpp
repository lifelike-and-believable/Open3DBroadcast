// Acceptance matrix for src/o3ds/predict/concealment.*, per the roadmap
// doc's C1 spec (docs/roadmap/resilient-streaming-and-motion-prediction.md,
// §5/C1).
#include "test_framework.h"

#include "o3ds/predict/concealment.h"
#include "o3ds/predict/hold_predictor.h"
#include "o3ds/predict/linear_predictor.h"

#include <cmath>
#include <memory>

using namespace O3DS;

namespace
{
	constexpr double kPi = 3.14159265358979323846;

	double QuatDot(const Quat& a, const Quat& b)
	{
		return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3];
	}

	bool IsUnitNorm(const Quat& q, double tol = 1.0e-9)
	{
		const double lenSq = q.v[0] * q.v[0] + q.v[1] * q.v[1] + q.v[2] * q.v[2] + q.v[3] * q.v[3];
		return std::abs(lenSq - 1.0) < tol;
	}

	bool QuatsNearlyEqual(const Quat& a, const Quat& b, double tol = 1.0e-6)
	{
		return std::abs(std::abs(QuatDot(a, b)) - 1.0) < tol;
	}

	// Ground truth: constant-velocity translation (x(t) = velocity*t) and
	// constant-angular-velocity rotation about Z (theta(t) = angularVel*t),
	// single node/channel each - just enough to exercise every field
	// BlendPoseSample/the predictors touch.
	PoseSample MakeSample(double t, uint64_t seq, double velocity, double angularVel)
	{
		PoseSample s;
		s.t = t;
		s.seq = seq;
		s.translations.push_back(Vector3d(velocity * t, 0.0, 0.0));
		s.rotations.push_back(QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), angularVel * t));
		s.scales.push_back(Vector3d(1.0, 1.0, 1.0));
		s.curves.push_back(0.0f);
		return s;
	}
}

// ---------------------------------------------------------------------------
// QuatSlerpShortestPath
// ---------------------------------------------------------------------------

O3DS_TEST(QuatSlerp_AlphaZero_ReturnsFirstOperand)
{
	Quat q0 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.3);
	Quat q1 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.9);
	O3DS_CHECK(QuatsNearlyEqual(QuatSlerpShortestPath(q0, q1, 0.0), q0));
}

O3DS_TEST(QuatSlerp_AlphaOne_ReturnsSecondOperand)
{
	Quat q0 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.3);
	Quat q1 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.9);
	O3DS_CHECK(QuatsNearlyEqual(QuatSlerpShortestPath(q0, q1, 1.0), q1));
}

O3DS_TEST(QuatSlerp_AlphaHalf_MidpointAngleAndUnitNorm)
{
	Quat q0 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.2);
	Quat q1 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.8);
	Quat mid = QuatSlerpShortestPath(q0, q1, 0.5);
	O3DS_CHECK(IsUnitNorm(mid));
	O3DS_CHECK(QuatsNearlyEqual(mid, QuatFromAxisAngle(Vector3d(0, 0, 1), 0.5)));
}

O3DS_TEST(QuatSlerp_AlphaOutsideUnitRange_ExtrapolatesAtConstantAngularVelocity)
{
	// The header explicitly documents that alpha isn't clamped - callers
	// extrapolating (e.g. LinearPredictor) rely on that.
	Quat q0 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.2);
	Quat q1 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.4);
	Quat beyond = QuatSlerpShortestPath(q0, q1, 2.0); // delta doubled past q1
	O3DS_CHECK(IsUnitNorm(beyond));
	O3DS_CHECK(QuatsNearlyEqual(beyond, QuatFromAxisAngle(Vector3d(0, 0, 1), 0.6)));
}

O3DS_TEST(QuatSlerp_NearIdentityDelta_ReturnsFirstOperand)
{
	Quat q0 = QuatFromAxisAngle(Vector3d(0, 0, 1), 0.5);
	Quat q1 = q0; // zero delta -> QuatToAxisAngle(dq) returns false (near-identity)
	O3DS_CHECK(QuatsNearlyEqual(QuatSlerpShortestPath(q0, q1, 0.5), q0));
}

// ---------------------------------------------------------------------------
// BlendPoseSample
// ---------------------------------------------------------------------------

O3DS_TEST(BlendPoseSample_AlphaZeroAndOne_ReturnsEndpoints)
{
	PoseSample from = MakeSample(0.0, 1, 10.0, 2.0);
	PoseSample to = MakeSample(0.0, 2, 10.0, 2.0);
	to.translations[0].v[0] = 5.0; // distinct from `from` regardless of shared t=0

	PoseSample atZero = BlendPoseSample(from, to, 0.0, 1.0, 42);
	PoseSample atOne = BlendPoseSample(from, to, 1.0, 1.0, 42);

	O3DS_CHECK(std::abs(atZero.translations[0].v[0] - from.translations[0].v[0]) < 1.0e-9);
	O3DS_CHECK(std::abs(atOne.translations[0].v[0] - to.translations[0].v[0]) < 1.0e-9);
	O3DS_CHECK_EQ(atZero.t, 1.0);
	O3DS_CHECK_EQ(atZero.seq, (uint64_t)42);
}

O3DS_TEST(BlendPoseSample_RotationStaysUnitNorm)
{
	PoseSample from = MakeSample(0.0, 1, 0.0, 0.0);
	PoseSample to = MakeSample(1.0, 2, 0.0, 1.2);
	PoseSample blended = BlendPoseSample(from, to, 0.3, 0.5, 1);
	O3DS_CHECK(IsUnitNorm(blended.rotations[0]));
}

// ---------------------------------------------------------------------------
// ConcealmentEngine
// ---------------------------------------------------------------------------

O3DS_TEST(ConcealmentEngine_NoHistory_TryConcealReturnsFalse)
{
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	PoseSample out;
	O3DS_CHECK(!engine.TryConceal(1.0, out));
}

O3DS_TEST(ConcealmentEngine_NoOpSafety_ZeroLoss_NeverConceals)
{
	// Every frame arrives on schedule (dt well under the starvation
	// threshold): TryConceal should never fire, so the caller's normal
	// real-frame path is byte-identical to today (roadmap's "no-op safety").
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	const double frameDt = 1.0 / 60.0;

	for (int i = 0; i < 10; ++i)
	{
		double t = i * frameDt;
		engine.ObserveRealFrame(MakeSample(t, (uint64_t)i, 5.0, 1.0));

		PoseSample out;
		O3DS_CHECK(!engine.TryConceal(t, out));
	}

	O3DS_CHECK_EQ(engine.Metrics().concealedFrameCount, (uint64_t)0);
	O3DS_CHECK_EQ(engine.Metrics().recoveryCount, (uint64_t)0);
}

O3DS_TEST(ConcealmentEngine_Starvation_PredictsExtrapolatedPose)
{
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	// Gap since last real frame (0.09 - 0.02 = 0.07s) exceeds the default
	// 0.05s starvation threshold -> should synthesize via the predictor.
	PoseSample out;
	O3DS_CHECK(engine.TryConceal(0.09, out));

	const double expectedX = 10.0 * 0.09; // constant-velocity ground truth
	O3DS_CHECK(std::abs(out.translations[0].v[0] - expectedX) < 1.0e-6);
	O3DS_CHECK_EQ(engine.Metrics().concealedFrameCount, (uint64_t)1);
	O3DS_CHECK_EQ(engine.Metrics().fallbackHoldCount, (uint64_t)0);
}

O3DS_TEST(ConcealmentEngine_SmallGap_DoesNotConceal)
{
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	// Gap of 0.03s is within the default 0.05s starvation threshold -
	// leave it to the caller's normal presentation path.
	PoseSample out;
	O3DS_CHECK(!engine.TryConceal(0.05, out));
	O3DS_CHECK_EQ(engine.Metrics().concealedFrameCount, (uint64_t)0);
}

O3DS_TEST(ConcealmentEngine_HorizonBound_StopsExtrapolatingAndHolds)
{
	ConcealmentConfig config;
	config.maxConcealHorizonSeconds = 0.15;

	ConcealmentEngine engine(std::make_unique<LinearPredictor>(), config);
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	PoseSample out;
	O3DS_CHECK(engine.TryConceal(0.09, out)); // starvation begins here (concealStartTime = 0.09)

	PoseSample beyondHorizon1;
	O3DS_CHECK(engine.TryConceal(0.09 + 0.20, beyondHorizon1)); // 0.20s of concealment > 0.15s horizon
	O3DS_CHECK_EQ(engine.Metrics().fallbackHoldCount, (uint64_t)1);

	PoseSample beyondHorizon2;
	O3DS_CHECK(engine.TryConceal(0.09 + 0.30, beyondHorizon2));

	// Held pose must not keep extrapolating once the horizon is exceeded -
	// same translation regardless of how much further tNow advances.
	O3DS_CHECK(std::abs(beyondHorizon1.translations[0].v[0] - beyondHorizon2.translations[0].v[0]) < 1.0e-9);
	O3DS_CHECK_EQ(engine.Metrics().fallbackHoldCount, (uint64_t)2);
}

O3DS_TEST(ConcealmentEngine_LinearPredictor_BeatsHold_OnRecoveryMetrics)
{
	// Same dropped-frame scenario run through both predictors: constant
	// velocity/angular-velocity ground truth means LinearPredictor should
	// recover almost exactly, while HoldPredictor (== today) freezes and
	// accumulates real error - directly validating C1's acceptance
	// criterion ("LinearPredictor error < HoldPredictor on the same
	// dropped frames").
	auto runScenario = [](std::unique_ptr<IPosePredictor> predictor) {
		ConcealmentEngine engine(std::move(predictor));
		engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
		engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

		PoseSample synthesized;
		engine.TryConceal(0.09, synthesized); // presentation tick during the gap

		// The dropped frames' true pose resumes at t=0.12 (a 0.10s gap).
		engine.ObserveRealFrame(MakeSample(0.12, 3, 10.0, 2.0));
		return engine.Metrics();
	};

	ConcealmentMetrics linearMetrics = runScenario(std::make_unique<LinearPredictor>());
	ConcealmentMetrics holdMetrics = runScenario(std::make_unique<HoldPredictor>());

	O3DS_CHECK_EQ(linearMetrics.recoveryCount, (uint64_t)1);
	O3DS_CHECK_EQ(holdMetrics.recoveryCount, (uint64_t)1);

	// Sanity: Hold actually has real error/pop in this scenario (otherwise
	// the comparison below would be meaningless).
	O3DS_CHECK(holdMetrics.MeanPredictionTranslationError() > 1.0e-3);
	O3DS_CHECK(holdMetrics.MeanPopTranslation() > 1.0e-3);

	O3DS_CHECK(linearMetrics.MeanPredictionTranslationError() < holdMetrics.MeanPredictionTranslationError());
	O3DS_CHECK(linearMetrics.MeanPredictionRotationErrorRadians() < holdMetrics.MeanPredictionRotationErrorRadians());
	O3DS_CHECK(linearMetrics.MeanPopTranslation() < holdMetrics.MeanPopTranslation());
	O3DS_CHECK(linearMetrics.MeanPopRotationRadians() < holdMetrics.MeanPopRotationRadians());

	// On this exactly-constant-velocity ground truth, Linear's recovery
	// error should be near zero (its model matches the true motion).
	O3DS_CHECK(linearMetrics.MeanPredictionTranslationError() < 1.0e-6);
}

O3DS_TEST(ConcealmentEngine_CorrectionWindow_BlendsThenStopsAfterWindowElapses)
{
	ConcealmentConfig config;
	config.correctionWindowSeconds = 0.10;

	ConcealmentEngine engine(std::make_unique<LinearPredictor>(), config);
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	PoseSample duringGap;
	engine.TryConceal(0.09, duringGap);
	engine.ObserveRealFrame(MakeSample(0.12, 3, 10.0, 2.0)); // recovery: starts the correction window at t=0.12

	// Real frames resume on a normal cadence for the rest of the window;
	// ordinary arrivals while correcting must not re-trigger a "recovery"
	// or restart the window (see ObserveRealFrame's mConcealing-only gate).
	engine.ObserveRealFrame(MakeSample(0.14, 4, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.16, 5, 10.0, 2.0));

	// Still within the correction window (elapsed = 0.17-0.12 = 0.05 <
	// 0.10s): keeps synthesizing a blended frame rather than snapping
	// straight to real.
	PoseSample corrected;
	O3DS_CHECK(engine.TryConceal(0.17, corrected));
	O3DS_CHECK(engine.Metrics().correctionFrameCount >= (uint64_t)1);
	O3DS_CHECK_EQ(engine.Metrics().recoveryCount, (uint64_t)1); // still just the one real recovery

	engine.ObserveRealFrame(MakeSample(0.18, 6, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.20, 7, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.22, 8, 10.0, 2.0));

	// Once the window fully elapses (elapsed = 0.23-0.12 = 0.11 > 0.10s)
	// and real data is genuinely flowing (gap = 0.23-0.22 = 0.01s), defers
	// back to the caller's normal real-frame path.
	PoseSample afterWindow;
	O3DS_CHECK(!engine.TryConceal(0.23, afterWindow));
	O3DS_CHECK_EQ(engine.Metrics().recoveryCount, (uint64_t)1); // never spuriously re-recorded
}

O3DS_TEST(ConcealmentEngine_CorrectionAlpha_ClampedAtZero_ForBackwardPresentationTime)
{
	// Regression: tNow behind mCorrectionStartTime is a normal, reachable
	// state under a buffered/offset presentation clock (t_pres intentionally
	// lags newest-received per A2.c) - an unclamped negative alpha would
	// extrapolate BlendPoseSample past the correction base, away from the
	// real pose it's supposed to be converging toward.
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	PoseSample duringGap;
	engine.TryConceal(0.09, duringGap); // mCorrectionBase will be this pose (x = 0.9)
	engine.ObserveRealFrame(MakeSample(0.12, 3, 10.0, 2.0)); // recovery starts correction at t=0.12

	// tNow (0.11) is behind mCorrectionStartTime (0.12) -> elapsed < 0.
	PoseSample corrected;
	O3DS_CHECK(engine.TryConceal(0.11, corrected));
	O3DS_CHECK(std::abs(corrected.translations[0].v[0] - duringGap.translations[0].v[0]) < 1.0e-9);
}

O3DS_TEST(ConcealmentEngine_ConcealedDuration_ClampedAtZero_ForOutOfOrderRealFrame)
{
	// Regression: real.t is the arriving frame's own timestamp, not tNow: a
	// late/out-of-order arrival predating mConcealStartTime (a lossy/
	// unordered transport doesn't guarantee non-decreasing arrival) must not
	// drive the running concealed-time total negative.
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	PoseSample out;
	O3DS_CHECK(engine.TryConceal(0.20, out)); // concealStartTime = 0.20

	engine.ObserveRealFrame(MakeSample(0.15, 3, 10.0, 2.0)); // predates concealStartTime
	O3DS_CHECK(engine.Metrics().concealedTimeSecondsTotal >= 0.0);
	O3DS_CHECK(engine.Metrics().concealedTimeSecondsMax >= 0.0);
}

O3DS_TEST(ConcealmentEngine_SecondGapMidCorrection_StartsFreshConcealmentAndRecordsBothRecoveries)
{
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	PoseSample out;
	engine.TryConceal(0.09, out);
	engine.ObserveRealFrame(MakeSample(0.12, 3, 10.0, 2.0)); // recovery #1, starts correction window

	// A fresh gap opens before the correction window (0.10s) elapses -
	// should cleanly transition into a new concealment rather than getting
	// stuck in the (now-abandoned) correction state.
	O3DS_CHECK(engine.TryConceal(0.20, out)); // gap since 0.12 = 0.08 > threshold(0.05)

	engine.ObserveRealFrame(MakeSample(0.24, 4, 10.0, 2.0)); // recovery #2
	O3DS_CHECK_EQ(engine.Metrics().recoveryCount, (uint64_t)2);
}

O3DS_TEST(ConcealmentEngine_Reset_ClearsHistoryButKeepsMetrics)
{
	ConcealmentEngine engine(std::make_unique<LinearPredictor>());
	engine.ObserveRealFrame(MakeSample(0.0, 1, 10.0, 2.0));
	engine.ObserveRealFrame(MakeSample(0.02, 2, 10.0, 2.0));

	PoseSample out;
	engine.TryConceal(0.09, out);
	engine.ObserveRealFrame(MakeSample(0.12, 3, 10.0, 2.0));
	O3DS_CHECK(engine.Metrics().recoveryCount > (uint64_t)0);

	engine.Reset();
	O3DS_CHECK(!engine.TryConceal(1.0, out)); // history cleared - insufficient again
	O3DS_CHECK(engine.Metrics().recoveryCount > (uint64_t)0); // metrics are session-level, not cleared
}
