// CORE-13: residual rotations. The encoder commits, and the decoder applies,
// the same normalized reconstruction (reference + float residual), so the
// decoded rotation stays a unit quaternion and both predictor histories stay
// equal; the actual rotation is encoded in the reference's hemisphere, so q
// and -q (the same rotation) are neither a large residual nor a resend; and
// curves commit the value the decoder reconstructs.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"
#include "o3ds/predict/residual_codec.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace O3DS;

namespace
{
	Subject* BuildRig(SubjectList& list, int bones, int curves)
	{
		Subject* subject = list.addSubject("Rig");
		for (int i = 0; i < bones; ++i)
		{
			Transform* t = subject->addTransform("Bone" + std::to_string(i), i - 1);
			t->transformOrder.push_back(O3DS::TTranslation);
			t->transformOrder.push_back(O3DS::TRotation);
		}
		for (int c = 0; c < curves; ++c)
		{
			subject->mCurveNames.push_back("Curve" + std::to_string(c));
			subject->mCurveValues.push_back(0.0f);
		}
		return subject;
	}

	void Animate(Subject* subject, double t)
	{
		for (size_t i = 0; i < subject->mTransforms.size(); ++i)
		{
			const double k = 0.3 + 0.17 * (double)i;
			// Non-uniform, changing axis: the hard case for component residuals.
			const Vector3d axis(std::sin(k * t), std::cos(0.7 * k * t), 0.5);
			subject->mTransforms[i]->rotation.value = QuatFromAxisAngle(axis, 1.5 * std::sin(k * t) + 0.2 * (double)i);
			subject->mTransforms[i]->translation.value = Vector3d(std::sin(t), (double)i, 0.0);
		}
		for (size_t c = 0; c < subject->mCurveValues.size(); ++c)
		{
			subject->mCurveValues[c] = (float)(0.5 + 0.5 * std::sin(2.3 * t + (double)c));
		}
	}

	double Norm(const Vector4d& q)
	{
		return std::sqrt(q.v[0] * q.v[0] + q.v[1] * q.v[1] + q.v[2] * q.v[2] + q.v[3] * q.v[3]);
	}

	//! Angle between two rotations, sign-aware, in radians.
	double AngleBetween(const Vector4d& a, const Vector4d& b)
	{
		const double dot = std::abs(a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3]) / (Norm(a) * Norm(b));
		return 2.0 * std::acos(std::min(1.0, dot));
	}

	size_t RotationEntries(const std::vector<char>& frame)
	{
		const auto* updates = O3DS::Data::GetSubjectList(frame.data() + 8)->updates();
		if (updates == nullptr || updates->size() == 0 || updates->Get(0)->rotation() == nullptr)
			return 0;
		return updates->Get(0)->rotation()->size();
	}
}

O3DS_TEST(ResidualRotation_StaysUnitAndBoundedWithoutKeyframes)
{
	SubjectList sender;
	Subject* subject = BuildRig(sender, 6, 4);
	// No cadence keyframes: only the first frames after the full Subject are
	// keyframes, so any drift would accumulate for the whole run.
	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, /*keyframeIntervalFrames*/ 0));

	std::vector<char> frame;
	Animate(subject, 0.0);
	O3DS_CHECK(subject->Serialize(frame, 0.0) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));

	double maxNormError = 0.0;
	double maxAngleEarly = 0.0;
	double maxAngleLate = 0.0;
	double maxCurveError = 0.0;
	constexpr int Frames = 3000;
	for (int i = 1; i <= Frames; ++i)
	{
		const double t = i / 60.0;
		Animate(subject, t);
		size_t count = 0;
		O3DS_CHECK(subject->SerializeUpdateResidual(frame, count, 1.0e-6, t) > 0);
		O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));
		Subject* received = receiver.findSubject("Rig");
		for (size_t n = 0; n < subject->mTransforms.size(); ++n)
		{
			const Vector4d& q = received->mTransforms[n]->rotation.value;
			maxNormError = std::max(maxNormError, std::abs(Norm(q) - 1.0));
			const double angle = AngleBetween(q, subject->mTransforms[n]->rotation.value);
			if (i <= 100)
				maxAngleEarly = std::max(maxAngleEarly, angle);
			else if (i > Frames - 100)
				maxAngleLate = std::max(maxAngleLate, angle);
		}
		for (size_t c = 0; c < subject->mCurveValues.size(); ++c)
		{
			maxCurveError = std::max(maxCurveError, (double)std::abs(received->mCurveValues[c] - subject->mCurveValues[c]));
		}
	}
	// Unit length every frame (before CORE-13 the norm drifted).
	O3DS_CHECK(maxNormError < 1.0e-12);
	// The reconstruction error does not grow over the run.
	O3DS_CHECK(maxAngleLate < 1.0e-4);
	O3DS_CHECK(maxAngleLate <= std::max(maxAngleEarly * 4.0, 1.0e-5));
	O3DS_CHECK(maxCurveError < 1.0e-5);
}

O3DS_TEST(ResidualRotation_HemisphereFlipIsNotAResidualOrAResend)
{
	SubjectList sender;
	Subject* subject = BuildRig(sender, 1, 0);
	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	const Quat q = QuatFromAxisAngle(Vector3d(0.2, 1.0, 0.3), 1.1);
	subject->mTransforms[0]->rotation.value = q;

	std::vector<char> frame;
	O3DS_CHECK(subject->Serialize(frame, 0.0) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));

	// The same rotation every frame, alternating its sign.
	for (int i = 1; i <= 20; ++i)
	{
		const double s = (i % 2 == 0) ? 1.0 : -1.0;
		subject->mTransforms[0]->rotation.value = Quat(s * q.v[0], s * q.v[1], s * q.v[2], s * q.v[3]);
		size_t count = 0;
		O3DS_CHECK(subject->SerializeUpdateResidual(frame, count, 1.0e-6, i / 60.0) > 0);
		if (i > 3) // past the keyframes the encoder sends while it builds history
			O3DS_CHECK(RotationEntries(frame) == 0);
		O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));
		O3DS_CHECK(AngleBetween(receiver.findSubject("Rig")->mTransforms[0]->rotation.value, q) < 1.0e-5);
	}

	// The plain delta path: a sign flip alone is not a change (TransformRotation::delta()).
	SubjectList plain;
	Subject* other = BuildRig(plain, 1, 0);
	other->mTransforms[0]->rotation.value = q;
	size_t count = 0;
	O3DS_CHECK(other->SerializeUpdate(frame, count, 1.0e-6, 0.0) > 0); // sends q, marks it sent
	other->mTransforms[0]->rotation.value = Quat(-q.v[0], -q.v[1], -q.v[2], -q.v[3]);
	O3DS_CHECK(other->SerializeUpdate(frame, count, 1.0e-6, 0.02) > 0);
	O3DS_CHECK(RotationEntries(frame) == 0);
}
