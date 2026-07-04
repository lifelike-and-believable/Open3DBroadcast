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
	PoseSample s = MakeSample(0.0, 1, 10.0, 2.0);
	encoder.BeginFrame(s);
	O3DS_CHECK(encoder.IsKeyframe());
	encoder.Commit(s);
}

O3DS_TEST(ResidualEncoder_LinearPredictor_BecomesNonKeyframeOnceHistoryExists)
{
	// BeginFrame() decides using history committed so far, NOT this
	// frame's own value (the reference must only depend on PRIOR frames,
	// or the receiver could never reproduce it) - so LinearPredictor's "2
	// samples" needs TWO PRIOR Commit() calls, i.e. this is the 3rd
	// BeginFrame() call, not the 2nd.
	ResidualEncoder encoder(ResidualPredictorId::Linear);
	PoseSample s0 = MakeSample(0.0, 1, 10.0, 2.0);
	encoder.BeginFrame(s0); // keyframe (no history at all)
	encoder.Commit(s0);
	PoseSample s1 = MakeSample(0.02, 2, 10.0, 2.0);
	encoder.BeginFrame(s1); // still keyframe (only 1 prior commit)
	encoder.Commit(s1);
	PoseSample s2 = MakeSample(0.04, 3, 10.0, 2.0);
	encoder.BeginFrame(s2); // now 2 prior commits - Linear can predict

	O3DS_CHECK(!encoder.IsKeyframe());
	// Reference should match constant-velocity extrapolation to t=0.04:
	// exactly matches the actual value in this constant-velocity scenario.
	O3DS_CHECK(NearlyEqual(encoder.Reference().translations[0].v[0], 10.0 * 0.04, 1.0e-6));
	encoder.Commit(s2);
}

O3DS_TEST(ResidualEncoder_HoldPredictor_ReferenceEqualsLastCommittedSample)
{
	// This is the claim the roadmap makes for why C2 is a strict
	// generalization of today's scheme: HoldPredictor's Predict() always
	// returns the last committed sample, so residual-vs-Hold-reference is
	// identical to "actual minus last-sent" (today's legacy delta).
	ResidualEncoder encoder(ResidualPredictorId::Hold);
	PoseSample s0 = MakeSample(0.0, 1, 10.0, 2.0);
	encoder.BeginFrame(s0);
	encoder.Commit(s0);
	PoseSample s1 = MakeSample(0.02, 2, 10.0, 2.0);
	encoder.BeginFrame(s1);

	O3DS_CHECK(!encoder.IsKeyframe());
	O3DS_CHECK(NearlyEqual(encoder.Reference().translations[0].v[0], 10.0 * 0.0, 1.0e-9)); // == frame 0's committed value (the "last sent")
	encoder.Commit(s1);
}

O3DS_TEST(ResidualEncoder_KeyframeCadence_ForcesPeriodicKeyframe)
{
	// First two calls are keyframes purely from insufficient history
	// (Linear needs 2 PRIOR samples - see
	// BecomesNonKeyframeOnceHistoryExists above for why); the cadence
	// counter only starts accumulating once real predictions begin, at
	// the 3rd call.
	ResidualEncoder encoder(ResidualPredictorId::Linear, /*keyframeIntervalFrames*/ 3);

	auto step = [&](double t, uint64_t seq, bool expectKeyframe) {
		PoseSample s = MakeSample(t, seq, 10.0, 2.0);
		encoder.BeginFrame(s);
		O3DS_CHECK_EQ(encoder.IsKeyframe(), expectKeyframe);
		encoder.Commit(s);
	};

	step(0.00, 1, true);  // keyframe (no history at all)
	step(0.02, 2, true);  // keyframe (only 1 prior sample)
	step(0.04, 3, false); // residual (2 prior samples now available)
	step(0.06, 4, false); // residual (1 frame since keyframe)
	step(0.08, 5, false); // residual (2 frames since keyframe)
	step(0.10, 6, true);  // cadence elapsed (3 frames since keyframe) -> forced keyframe
	step(0.12, 7, false); // resets - back to residual
}

O3DS_TEST(ResidualDecoder_MirrorsEncoder_RoundTripReconstructsActualValue)
{
	// The actual C2 contract: feed the same sample sequence through both
	// sides; the caller computes residual = actual - encoder.Reference()
	// (or absolute if keyframe) and reconstructed = decoder.Reference() +
	// residual (or absolute if keyframe) - reconstructed must equal
	// actual, and BOTH sides must Commit()/EndFrame() with that same
	// reconstructed value (not the raw `actual`) to stay in lockstep -
	// trivially true here since nothing is ever omitted (no
	// deltaThreshold gate in this test), but exercised properly by
	// model_residual_tests.cpp's full wire round-trip.
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

		encoder.Commit(actual);
		decoder.EndFrame(actual); // fully reconstructed pose == actual in this test (nothing omitted)
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

O3DS_TEST(ResidualEncoder_Commit_WithOmittedChannelValue_StaysInLockstepWithDecoder)
{
	// Regression for the real bug an adversarial review found: if the
	// encoder observed the true `actual` value for a channel the caller
	// chose NOT to send (because its residual fell under some threshold),
	// its predictor history would silently diverge from the decoder's
	// (which only ever learns about a channel's reference value when it's
	// omitted) - and once the two sides' histories differ, EVEN A
	// SUBSEQUENTLY-SENT residual reconstructs wrong, since predicted +
	// residual only equals actual when both sides computed the identical
	// prediction from identical history.
	//
	// Simulates a caller-side omission by Commit()-ing the *reference*
	// value (what a receiver reconstructs when nothing is sent) instead
	// of the true, slightly-different actual value, for several frames in
	// a row - then verifies encoder and decoder still agree afterward.
	ResidualEncoder encoder(ResidualPredictorId::Linear);
	ResidualDecoder decoder(ResidualPredictorId::Linear);

	PoseSample s0 = MakeSample(0.00, 1, 10.0, 0.0);
	encoder.BeginFrame(s0);
	encoder.Commit(s0);
	decoder.BeginFrame(true, 0.00);
	decoder.EndFrame(s0);

	PoseSample s1 = MakeSample(0.02, 2, 10.0, 0.0);
	encoder.BeginFrame(s1);
	encoder.Commit(s1);
	decoder.BeginFrame(true, 0.02);
	decoder.EndFrame(s1);

	// From here on, Linear has 2 prior samples and predicts exactly
	// (constant velocity) - simulate 5 frames where the "true" value
	// drifts slightly off the prediction (a small perturbation), but the
	// caller decides to omit it every time (as if it were under
	// threshold), so both sides commit/end-frame with the REFERENCE
	// value, not the perturbed true one.
	double lastReference = 0.0;
	for (int i = 2; i < 7; ++i)
	{
		double t = i * 0.02;
		PoseSample truth = MakeSample(t, (uint64_t)i, 10.0, 0.0);
		truth.translations[0].v[0] += 1.0e-4; // small perturbation, "omitted" by the caller

		encoder.BeginFrame(truth);
		O3DS_CHECK(!encoder.IsKeyframe());
		PoseSample reference = encoder.Reference(); // what the decoder will also compute
		lastReference = reference.translations[0].v[0];
		encoder.Commit(reference); // omitted: commit the reference, not `truth`

		decoder.BeginFrame(false, t);
		O3DS_CHECK(!decoder.IsKeyframe());
		O3DS_CHECK(NearlyEqual(decoder.Reference().translations[0].v[0], reference.translations[0].v[0], 1.0e-9));
		decoder.EndFrame(reference);
	}

	// Now send a real residual and confirm reconstruction is still exact
	// - this is what would fail (reconstruct to the wrong value) if the
	// encoder had instead committed `truth` every frame above while the
	// decoder committed `reference`.
	double t = 7 * 0.02;
	PoseSample finalTruth = MakeSample(t, 7, 10.0, 0.0);
	encoder.BeginFrame(finalTruth);
	double residualX = finalTruth.translations[0].v[0] - encoder.Reference().translations[0].v[0];

	decoder.BeginFrame(false, t);
	double reconstructedX = decoder.Reference().translations[0].v[0] + residualX;

	O3DS_CHECK(NearlyEqual(reconstructedX, finalTruth.translations[0].v[0], 1.0e-9));
	(void)lastReference;
}

O3DS_TEST(ResidualEncoder_TopologyChange_ForcesFreshKeyframeInsteadOfMisalignedReference)
{
	// Regression: a bone count change without an explicit reset must not
	// let a stale (old-topology) Reference() get indexed against the new
	// topology - BeginFrame() must detect the channel-count mismatch
	// itself and force a keyframe.
	ResidualEncoder encoder(ResidualPredictorId::Linear);

	PoseSample s0 = MakeSample(0.00, 1, 10.0, 2.0); // 1 translation/rotation channel
	encoder.BeginFrame(s0);
	encoder.Commit(s0);
	PoseSample s1 = MakeSample(0.02, 2, 10.0, 2.0);
	encoder.BeginFrame(s1);
	encoder.Commit(s1);
	PoseSample s2 = MakeSample(0.04, 3, 10.0, 2.0);
	encoder.BeginFrame(s2);
	O3DS_CHECK(!encoder.IsKeyframe()); // predicting normally now
	encoder.Commit(s2);

	// Topology change: a second bone appears.
	PoseSample grown = MakeSample(0.06, 4, 10.0, 2.0);
	grown.translations.push_back(Vector3d(1.0, 2.0, 3.0));
	grown.rotations.push_back(QuatFromAxisAngle(Vector3d(0, 0, 1), 0.5));
	grown.scales.push_back(Vector3d(1.0, 1.0, 1.0));

	encoder.BeginFrame(grown);
	O3DS_CHECK(encoder.IsKeyframe()); // forced, not a stale 1-channel reference misapplied to 2 channels
	O3DS_CHECK_EQ(encoder.Reference().translations.size(), (size_t)0); // empty - keyframe, nothing to index into
	encoder.Commit(grown);

	// Recovers normally afterward on the new topology.
	PoseSample grown2 = MakeSample(0.08, 5, 10.0, 2.0);
	grown2.translations.push_back(Vector3d(1.1, 2.0, 3.0));
	grown2.rotations.push_back(QuatFromAxisAngle(Vector3d(0, 0, 1), 0.5));
	grown2.scales.push_back(Vector3d(1.0, 1.0, 1.0));
	encoder.BeginFrame(grown2);
	O3DS_CHECK(encoder.IsKeyframe()); // still only 1 prior sample on the new topology
	encoder.Commit(grown2);
}
