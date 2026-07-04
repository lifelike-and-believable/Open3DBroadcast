// Full Subject/SubjectList wire round-trip tests for C2 residual coding
// (roadmap doc §5/C2), layered on top of model_residual_tests.cpp's
// PoseSample-level sibling residual_codec_tests.cpp. Exercises the actual
// FlatBuffers SerializeUpdateResidual()/ParseUpdateResidual() path, not
// just the predictor-agnostic ResidualEncoder/Decoder in isolation.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"

#include <algorithm>
#include <cmath>
#include <memory>

using namespace O3DS;

namespace
{
	void BuildSkeleton(SubjectList& subjects, const std::string& name)
	{
		auto* subject = subjects.addSubject(name);
		subject->mCurveNames = { "Smile", "Blink" };
		subject->mCurveValues = { 0.0f, 0.0f };

		auto* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);

		auto* spine = subject->addTransform("Spine", 0);
		spine->transformOrder.push_back(O3DS::TTranslation);
		spine->transformOrder.push_back(O3DS::TRotation);

		auto* head = subject->addTransform("Head", 1);
		head->transformOrder.push_back(O3DS::TTranslation);
		head->transformOrder.push_back(O3DS::TRotation);
	}

	// Distinct per-node constant-velocity/angular-velocity ground truth -
	// close to concealment_tests.cpp's own MakeSample() convention, just
	// applied directly onto a live Subject's transforms instead of a
	// standalone PoseSample.
	void ApplyMotion(Subject* subject, double t)
	{
		static const double velocities[3] = { 1.0, 2.0, -1.5 };
		static const double angularVels[3] = { 0.5, -0.3, 1.1 };

		// Only the original 3-bone BuildSkeleton() topology has ground
		// truth here; a topology-change test appends extra bones and sets
		// their motion separately.
		const size_t n = std::min(subject->mTransforms.size(), (size_t)3);
		for (size_t i = 0; i < n; ++i)
		{
			subject->mTransforms[i]->translation.value = Vector3d(velocities[i] * t, velocities[i] * t * 0.5, 0.0);
			subject->mTransforms[i]->rotation.value = QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), angularVels[i] * t);
		}
		subject->mCurveValues[0] = (float)(0.1 * t);
		subject->mCurveValues[1] = (float)std::sin(t);
	}

	bool NearlyEqual(double a, double b, double tol)
	{
		return std::abs(a - b) < tol;
	}
}

O3DS_TEST(ResidualRoundTrip_ReconstructsMotionAcrossManyFrames)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	Subject* senderSubject = sender.findSubject("Actor");
	senderSubject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	sender.SetDeltaThreshold(1.0e-6);

	// Establish topology + initial values on the receiver via a full sync,
	// same as any real session's first frame.
	std::vector<char> fullBuf;
	O3DS_CHECK(sender.Serialize(fullBuf) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(fullBuf.data(), fullBuf.size()));
	Subject* receiverSubject = receiver.findSubject("Actor");
	O3DS_CHECK(receiverSubject != nullptr);
	receiverSubject->SetResidualDecoder(std::make_unique<ResidualDecoder>(ResidualPredictorId::Linear));

	for (int i = 1; i <= 50; ++i)
	{
		double t = i * 0.02;
		ApplyMotion(senderSubject, t);

		size_t count = 0;
		std::vector<char> buf;
		O3DS_CHECK(sender.SerializeUpdateResidual(buf, count, t) > 0);
		O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

		for (size_t n = 0; n < senderSubject->mTransforms.size(); ++n)
		{
			O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[n]->translation.value.v[0], senderSubject->mTransforms[n]->translation.value.v[0], 1.0e-4));
			O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[n]->translation.value.v[1], senderSubject->mTransforms[n]->translation.value.v[1], 1.0e-4));
			O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[n]->rotation.value.v[3], senderSubject->mTransforms[n]->rotation.value.v[3], 1.0e-4));
		}
		O3DS_CHECK(NearlyEqual(receiverSubject->mCurveValues[0], senderSubject->mCurveValues[0], 1.0e-4));
		O3DS_CHECK(NearlyEqual(receiverSubject->mCurveValues[1], senderSubject->mCurveValues[1], 1.0e-4));
	}
}

O3DS_TEST(ResidualRoundTrip_WithPeriodicKeyframes_StaysCorrect)
{
	// Exercises the "cadence-forced keyframe" path repeatedly within one
	// live session - not just the very first frame, which every other
	// test already covers implicitly.
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	Subject* senderSubject = sender.findSubject("Actor");
	senderSubject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, /*keyframeIntervalFrames*/ 5));
	sender.SetDeltaThreshold(1.0e-6);

	std::vector<char> fullBuf;
	O3DS_CHECK(sender.Serialize(fullBuf) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(fullBuf.data(), fullBuf.size()));
	Subject* receiverSubject = receiver.findSubject("Actor");
	receiverSubject->SetResidualDecoder(std::make_unique<ResidualDecoder>(ResidualPredictorId::Linear));

	for (int i = 1; i <= 30; ++i)
	{
		double t = i * 0.02;
		ApplyMotion(senderSubject, t);

		size_t count = 0;
		std::vector<char> buf;
		O3DS_CHECK(sender.SerializeUpdateResidual(buf, count, t) > 0);
		O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

		for (size_t n = 0; n < senderSubject->mTransforms.size(); ++n)
		{
			O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[n]->translation.value.v[0], senderSubject->mTransforms[n]->translation.value.v[0], 1.0e-4));
		}
	}
}

O3DS_TEST(ResidualWithHoldPredictor_MatchesLegacyDeltaScheme)
{
	// The roadmap's own claim: HoldPredictor residual coding "reduces
	// exactly to today's delta scheme" - run the same motion through both
	// the legacy path and Hold-based residual coding, confirm they land on
	// the same reconstructed value. Includes a slow-drift channel (Head,
	// index 2) whose per-frame movement is deliberately tiny so BOTH
	// schemes' omission logic gets exercised for many consecutive frames
	// (not just "everything always sent") - this is what makes the
	// comparison a real equivalence proof rather than trivially true.
	auto runScenario = [](bool useResidual) {
		SubjectList sender;
		BuildSkeleton(sender, "Actor");
		Subject* senderSubject = sender.findSubject("Actor");
		if (useResidual)
		{
			senderSubject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Hold));
		}
		sender.SetDeltaThreshold(1.0e-3);

		std::vector<char> fullBuf;
		sender.Serialize(fullBuf);
		SubjectList receiver;
		receiver.Parse(fullBuf.data(), fullBuf.size());
		Subject* receiverSubject = receiver.findSubject("Actor");
		if (useResidual)
		{
			receiverSubject->SetResidualDecoder(std::make_unique<ResidualDecoder>(ResidualPredictorId::Hold));
		}

		for (int i = 1; i <= 200; ++i)
		{
			double t = i * 0.02;
			ApplyMotion(senderSubject, t);
			// Head (index 2) drifts by a tiny amount per frame - well
			// under deltaThreshold=1e-3 on any single frame, so both
			// schemes omit it repeatedly and it only actually moves via
			// many accumulated omitted-frame carry-overs.
			senderSubject->mTransforms[2]->translation.value.v[0] += 1.0e-5 * i;

			size_t count = 0;
			std::vector<char> buf;
			if (useResidual)
				sender.SerializeUpdateResidual(buf, count, t);
			else
				sender.SerializeUpdate(buf, count, t);

			receiver.Parse(buf.data(), buf.size());
		}

		return receiverSubject->mTransforms[2]->translation.value.v[0];
	};

	double legacyResult = runScenario(false);
	double residualResult = runScenario(true);

	O3DS_CHECK(NearlyEqual(legacyResult, residualResult, 1.0e-6));
}

O3DS_TEST(ResidualRoundTrip_NoisyMotionWithRealisticThreshold_StaysBoundedOverManyFrames)
{
	// Regression for the real bug an adversarial review found: with
	// non-constant-velocity motion (so LinearPredictor's residual is
	// genuinely nonzero, not just float noise) and a realistic
	// deltaThreshold (not 1e-6), many frames' residuals fall BELOW
	// threshold and get omitted while the value is still truly moving.
	// Before the fix, ParseUpdateResidual only touched wire-present
	// channels, so an omitted-but-moving channel would freeze; encoder
	// also had to Commit() the RECONSTRUCTED pose (not the true actual)
	// or the two sides' predictor histories would silently diverge and
	// even later *sent* residuals would reconstruct wrong. Runs 300
	// frames with keyframes disabled entirely (interval=0) specifically
	// so there's no periodic re-anchor masking a divergence bug.
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	Subject* senderSubject = sender.findSubject("Actor");
	senderSubject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, /*keyframeIntervalFrames*/ 0));
	sender.SetDeltaThreshold(2.0e-3);

	std::vector<char> fullBuf;
	O3DS_CHECK(sender.Serialize(fullBuf) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(fullBuf.data(), fullBuf.size()));
	Subject* receiverSubject = receiver.findSubject("Actor");
	receiverSubject->SetResidualDecoder(std::make_unique<ResidualDecoder>(ResidualPredictorId::Linear));

	for (int i = 1; i <= 300; ++i)
	{
		double t = i * 0.02;
		ApplyMotion(senderSubject, t);
		// Curved (non-constant-velocity) perturbation on top of the base
		// linear motion, small enough that LinearPredictor's per-frame
		// error is mostly sub-threshold: sin() gives a smoothly varying,
		// genuinely nonzero prediction error every frame (not the ~0
		// float noise a pure constant-velocity scenario would give).
		senderSubject->mTransforms[0]->translation.value.v[0] += 5.0e-4 * std::sin(t * 3.0);
		senderSubject->mTransforms[1]->rotation.value = QuatMultiply(
			senderSubject->mTransforms[1]->rotation.value,
			QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), 1.0e-4 * std::sin(t * 5.0)));

		size_t count = 0;
		std::vector<char> buf;
		O3DS_CHECK(sender.SerializeUpdateResidual(buf, count, t) > 0);
		O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

		// Bounded, not diverging: the per-frame perturbation is ~5e-4 at
		// most, so a healthy (non-diverging) reconstruction should track
		// within a small constant multiple of deltaThreshold, not grow
		// with frame count. A divergence bug would show this error
		// growing roughly linearly with `i`.
		for (size_t n = 0; n < senderSubject->mTransforms.size(); ++n)
		{
			O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[n]->translation.value.v[0], senderSubject->mTransforms[n]->translation.value.v[0], 1.0e-2));
			O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[n]->rotation.value.v[3], senderSubject->mTransforms[n]->rotation.value.v[3], 1.0e-2));
		}
	}
}

O3DS_TEST(ResidualRoundTrip_TopologyChangeMidStream_ResyncsWithoutMisalignment)
{
	// Regression for the topology-change safety gap an adversarial review
	// found: a bone added mid-stream, synced via a normal full
	// Subject::Serialize()/ParseSubject() resync (exactly how a real
	// skeleton-change event reaches the wire today), must not let a
	// stale (old-topology) residual reference get misapplied to the
	// wrong channel under the new topology - both the sender's encoder
	// (self-detecting via ToPoseSample()'s own channel counts) and the
	// receiver's decoder (reset by ParseSubject on any full resync) must
	// force a keyframe and recover cleanly.
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	Subject* senderSubject = sender.findSubject("Actor");
	senderSubject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	sender.SetDeltaThreshold(1.0e-6);

	std::vector<char> fullBuf;
	O3DS_CHECK(sender.Serialize(fullBuf) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(fullBuf.data(), fullBuf.size()));
	Subject* receiverSubject = receiver.findSubject("Actor");
	receiverSubject->SetResidualDecoder(std::make_unique<ResidualDecoder>(ResidualPredictorId::Linear));

	// Warm up on the original 3-bone topology until predictions are real
	// (not just insufficient-history keyframes).
	for (int i = 1; i <= 5; ++i)
	{
		double t = i * 0.02;
		ApplyMotion(senderSubject, t);
		size_t count = 0;
		std::vector<char> buf;
		O3DS_CHECK(sender.SerializeUpdateResidual(buf, count, t) > 0);
		O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));
	}

	// Topology change: a 4th bone appears. Re-serialize the FULL subject
	// (not an update) to sync it, matching how a real skeleton change
	// reaches the wire - this is what triggers ParseSubject() on the
	// receiver, which must reset its residual decoder.
	senderSubject->addTransform("LeftHand", 2);
	senderSubject->mTransforms[3]->translation.value = Vector3d(5.0, 5.0, 5.0);
	senderSubject->mTransforms[3]->rotation.value = QuatIdentity();
	senderSubject->mTransforms[3]->transformOrder.push_back(O3DS::TTranslation);
	senderSubject->mTransforms[3]->transformOrder.push_back(O3DS::TRotation);

	std::vector<char> resyncBuf;
	O3DS_CHECK(sender.Serialize(resyncBuf) > 0);
	O3DS_CHECK(receiver.Parse(resyncBuf.data(), resyncBuf.size()));

	// A full resync with the default clearInactive=true deletes and
	// recreates every tracked Subject (so a stale one never leaks) - the
	// old `receiverSubject` pointer is now dangling; re-fetch it.
	receiverSubject = receiver.findSubject("Actor");
	O3DS_CHECK(receiverSubject != nullptr);
	O3DS_CHECK_EQ(receiverSubject->mTransforms.size(), (size_t)4);

	// Continue residual updates on the new (4-bone) topology - the
	// encoder detects the channel-count mismatch itself on the very next
	// SerializeUpdateResidual() call and forces a keyframe; the decoder
	// was already reset by ParseSubject() above. Neither side should
	// misalign channel 3's (the new bone's) reference against a stale
	// 3-channel history, and reconstruction should track correctly from
	// here on.
	for (int i = 6; i <= 20; ++i)
	{
		double t = i * 0.02;
		ApplyMotion(senderSubject, t);
		senderSubject->mTransforms[3]->translation.value = Vector3d(5.0 + 0.1 * i, 5.0, 5.0);

		size_t count = 0;
		std::vector<char> buf;
		O3DS_CHECK(sender.SerializeUpdateResidual(buf, count, t) > 0);
		O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

		for (size_t n = 0; n < senderSubject->mTransforms.size(); ++n)
		{
			O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[n]->translation.value.v[0], senderSubject->mTransforms[n]->translation.value.v[0], 1.0e-4));
		}
	}
}

O3DS_TEST(LegacyUpdate_StillDispatchesCorrectly_RegressionForPredictorIdSwitch)
{
	// Regression: Parse()'s new predictor_id-based dispatch must not
	// change behavior for a legacy sender at all (predictor_id defaults
	// to 0/None when never set, exactly like an old sender).
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	Subject* senderSubject = sender.findSubject("Actor");
	sender.SetDeltaThreshold(1.0e-6);

	std::vector<char> fullBuf;
	sender.Serialize(fullBuf);
	SubjectList receiver;
	receiver.Parse(fullBuf.data(), fullBuf.size());
	Subject* receiverSubject = receiver.findSubject("Actor");

	ApplyMotion(senderSubject, 1.0);

	size_t count = 0;
	std::vector<char> buf;
	sender.SerializeUpdate(buf, count, 1.0);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	O3DS_CHECK(NearlyEqual(receiverSubject->mTransforms[0]->translation.value.v[0], senderSubject->mTransforms[0]->translation.value.v[0], 1.0e-6));
}
