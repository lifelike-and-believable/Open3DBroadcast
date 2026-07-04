// Acceptance matrix for src/o3ds/predict/residual_codec.*, per the roadmap
// doc's C2 spec (docs/roadmap/resilient-streaming-and-motion-prediction.md,
// §5/C2). PoseSample-level only - see model_residual_tests.cpp for the
// full Subject/SubjectList wire round-trip.
#include "test_framework.h"

#include "o3ds/predict/residual_codec.h"
#include "o3ds/predict/hold_predictor.h"

#include <cmath>

using namespace O3DS;

namespace
{
	PoseSample MakeSample(double t, uint64_t seq, double velocity, double angularVel)
	{
		PoseSample s;
		s.t = t;
		s.seq = seq;
		s.translations.push_back(Vector3d(velocity * t, 0.0, 0.0));
		s.rotations.push_back(QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), angularVel * t));
		s.scales.push_back(Vector3d(1.0, 1.0, 1.0));
		s.curves.push_back((float)(0.1 * t));
		return s;
	}

	bool NearlyEqual(double a, double b, double tol = 1.0e-9)
	{
		return std::abs(a - b) < tol;
	}
}

O3DS_TEST(MakePredictorForId_NoneReturnsNull)
{
	O3DS_CHECK(MakePredictorForId(ResidualPredictorId::None) == nullptr);
}

O3DS_TEST(MakePredictorForId_ConstructsExpectedVersions)
{
	auto hold = MakePredictorForId(ResidualPredictorId::Hold);
	O3DS_CHECK(hold != nullptr);
	auto linear = MakePredictorForId(ResidualPredictorId::Linear);
	O3DS_CHECK(linear != nullptr);
	auto quadratic = MakePredictorForId(ResidualPredictorId::Quadratic);
	O3DS_CHECK(quadratic != nullptr);
}

O3DS_TEST(ResidualEncoder_FirstFrame_IsAlwaysKeyframe)
{
	ResidualEncoder encoder(ResidualPredictorId::Linear);
	encoder.BeginFrame(MakeSample(0.0, 1, 10.0, 2.0));
	O3DS_CHECK(encoder.IsKeyframe());
}

O3DS_TEST(ResidualEncoder_LinearPredictor_BecomesNonKeyframeOnceHistoryExists)
{
	// BeginFrame() calls Predict() BEFORE Observe()-ing the current sample
	// (the reference must only depend on PRIOR frames, or the receiver
	// could never reproduce it) - so LinearPredictor's "2 samples" needs
	// TWO PRIOR frames observed, i.e. this is the 3rd BeginFrame() call,
	// not the 2nd.
	ResidualEncoder encoder(ResidualPredictorId::Linear);
	encoder.BeginFrame(MakeSample(0.0, 1, 10.0, 2.0)); // keyframe (no history at all)
	encoder.BeginFrame(MakeSample(0.02, 2, 10.0, 2.0)); // still keyframe (only 1 prior sample)
	encoder.BeginFrame(MakeSample(0.04, 3, 10.0, 2.0)); // now 2 prior samples - Linear can predict

	O3DS_CHECK(!encoder.IsKeyframe());
	// Reference should match constant-velocity extrapolation to t=0.04:
	// exactly matches the actual value in this constant-velocity scenario.
	O3DS_CHECK(NearlyEqual(encoder.Reference().translations[0].v[0], 10.0 * 0.04, 1.0e-6));
}

O3DS_TEST(ResidualEncoder_HoldPredictor_ReferenceEqualsLastObservedSample)
{
	// This is the claim the roadmap makes for why C2 is a strict
	// generalization of today's scheme: HoldPredictor's Predict() always
	// returns the last Observe()'d sample, so residual-vs-Hold-reference
	// is identical to "actual minus last-sent" (today's legacy delta).
	ResidualEncoder encoder(ResidualPredictorId::Hold);
	encoder.BeginFrame(MakeSample(0.0, 1, 10.0, 2.0));
	encoder.BeginFrame(MakeSample(0.02, 2, 10.0, 2.0));

	O3DS_CHECK(!encoder.IsKeyframe());
	O3DS_CHECK(NearlyEqual(encoder.Reference().translations[0].v[0], 10.0 * 0.0, 1.0e-9)); // == frame 1's actual value (the "last sent")
}

O3DS_TEST(ResidualEncoder_KeyframeCadence_ForcesPeriodicKeyframe)
{
	// First two calls are keyframes purely from insufficient history
	// (Linear needs 2 PRIOR samples - see the
	// BecomesNonKeyframeOnceHistoryExists test above for why); the
	// cadence counter only starts accumulating once real predictions
	// begin, at the 3rd call.
	ResidualEncoder encoder(ResidualPredictorId::Linear, /*keyframeIntervalFrames*/ 3);

	encoder.BeginFrame(MakeSample(0.00, 1, 10.0, 2.0)); // keyframe (no history at all)
	O3DS_CHECK(encoder.IsKeyframe());
	encoder.BeginFrame(MakeSample(0.02, 2, 10.0, 2.0)); // keyframe (only 1 prior sample)
	O3DS_CHECK(encoder.IsKeyframe());
	encoder.BeginFrame(MakeSample(0.04, 3, 10.0, 2.0)); // residual (2 prior samples now available)
	O3DS_CHECK(!encoder.IsKeyframe());
	encoder.BeginFrame(MakeSample(0.06, 4, 10.0, 2.0)); // residual (1 frame since keyframe)
	O3DS_CHECK(!encoder.IsKeyframe());
	encoder.BeginFrame(MakeSample(0.08, 5, 10.0, 2.0)); // residual (2 frames since keyframe)
	O3DS_CHECK(!encoder.IsKeyframe());
	encoder.BeginFrame(MakeSample(0.10, 6, 10.0, 2.0)); // cadence elapsed (3 frames since keyframe) -> forced keyframe
	O3DS_CHECK(encoder.IsKeyframe());
	encoder.BeginFrame(MakeSample(0.12, 7, 10.0, 2.0)); // resets - back to residual
	O3DS_CHECK(!encoder.IsKeyframe());
}

O3DS_TEST(ResidualDecoder_MirrorsEncoder_RoundTripReconstructsActualValue)
{
	// The actual C2 contract: feed the same sample sequence through both
	// sides; the caller computes residual = actual - encoder.Reference()
	// (or absolute if keyframe) and reconstructed = decoder.Reference() +
	// residual (or absolute if keyframe) - reconstructed must equal actual.
	ResidualEncoder encoder(ResidualPredictorId::Linear);
	ResidualDecoder decoder(ResidualPredictorId::Linear);

	for (int i = 0; i < 20; ++i)
	{
		double t = i * 0.02;
		PoseSample actual = MakeSample(t, (uint64_t)i, 10.0, 2.0);

		encoder.BeginFrame(actual);
		decoder.BeginFrame(encoder.IsKeyframe(), t);
		O3DS_CHECK_EQ(decoder.IsKeyframe(), encoder.IsKeyframe());

		double refX = encoder.IsKeyframe() ? 0.0 : encoder.Reference().translations[0].v[0];
		double residualX = actual.translations[0].v[0] - refX;

		double decodedRefX = decoder.IsKeyframe() ? 0.0 : decoder.Reference().translations[0].v[0];
		double reconstructedX = decodedRefX + residualX;

		O3DS_CHECK(NearlyEqual(reconstructedX, actual.translations[0].v[0], 1.0e-9));

		decoder.EndFrame(actual); // fully reconstructed pose == actual in this test (no float rounding injected)
	}
}

O3DS_TEST(ResidualDecoder_InsufficientHistory_FallsBackToKeyframeEvenIfWireSaysResidual)
{
	ResidualDecoder decoder(ResidualPredictorId::Linear);
	// No BeginFrame()/EndFrame() yet - decoder has no history at all.
	decoder.BeginFrame(/*incomingIsKeyframe*/ false, 0.5);
	O3DS_CHECK(decoder.IsKeyframe()); // forced true despite the wire claiming residual mode
}

O3DS_TEST(ResidualDecoder_Reset_ClearsHistoryAndForcesKeyframeAgain)
{
	// Same "needs 2 PRIOR samples" timing as the encoder side: the 3rd
	// BeginFrame() is the first one where Predict() can actually succeed.
	ResidualDecoder decoder(ResidualPredictorId::Linear);
	decoder.BeginFrame(true, 0.0);
	decoder.EndFrame(MakeSample(0.0, 1, 10.0, 2.0));
	decoder.BeginFrame(true, 0.02); // still insufficient history (1 prior sample)
	decoder.EndFrame(MakeSample(0.02, 2, 10.0, 2.0));
	decoder.BeginFrame(false, 0.04); // 2 prior samples now available
	O3DS_CHECK(!decoder.IsKeyframe());
	decoder.EndFrame(MakeSample(0.04, 3, 10.0, 2.0));

	decoder.Reset();
	decoder.BeginFrame(false, 0.06); // wire claims residual, but history was just cleared
	O3DS_CHECK(decoder.IsKeyframe());
}
