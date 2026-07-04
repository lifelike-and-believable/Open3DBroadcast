// Full Subject/SubjectList wire round-trip tests for C2 residual coding
// (roadmap doc §5/C2), layered on top of model_residual_tests.cpp's
// PoseSample-level sibling residual_codec_tests.cpp. Exercises the actual
// FlatBuffers SerializeUpdateResidual()/ParseUpdateResidual() path, not
// just the predictor-agnostic ResidualEncoder/Decoder in isolation.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"

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

		for (size_t i = 0; i < subject->mTransforms.size(); ++i)
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
	// the same reconstructed value.
	auto runScenario = [](bool useResidual) {
		SubjectList sender;
		BuildSkeleton(sender, "Actor");
		Subject* senderSubject = sender.findSubject("Actor");
		if (useResidual)
		{
			senderSubject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Hold));
		}
		sender.SetDeltaThreshold(1.0e-6);

		std::vector<char> fullBuf;
		sender.Serialize(fullBuf);
		SubjectList receiver;
		receiver.Parse(fullBuf.data(), fullBuf.size());
		Subject* receiverSubject = receiver.findSubject("Actor");
		if (useResidual)
		{
			receiverSubject->SetResidualDecoder(std::make_unique<ResidualDecoder>(ResidualPredictorId::Hold));
		}

		for (int i = 1; i <= 20; ++i)
		{
			double t = i * 0.02;
			ApplyMotion(senderSubject, t);

			size_t count = 0;
			std::vector<char> buf;
			if (useResidual)
				sender.SerializeUpdateResidual(buf, count, t);
			else
				sender.SerializeUpdate(buf, count, t);

			receiver.Parse(buf.data(), buf.size());
		}

		return receiverSubject->mTransforms[0]->translation.value.v[0];
	};

	double legacyResult = runScenario(false);
	double residualResult = runScenario(true);

	O3DS_CHECK(NearlyEqual(legacyResult, residualResult, 1.0e-6));
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
