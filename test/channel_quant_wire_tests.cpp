// Full Subject/SubjectList wire round-trip tests for D1 adaptive channel
// quantization (roadmap doc §6/D1), layered on top of channel_quant_tests.cpp's
// pure-math codec tests. Exercises the actual SerializeUpdate()/ParseUpdate()
// integration - schema fields, rest-pose anchor capture/reconstruction, and
// tier fallback - not just QuantizeByte/QuantizeRotationByte in isolation.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"

#include <cmath>

using namespace O3DS;

namespace
{
	void BuildSkeleton(SubjectList& subjects, const std::string& name)
	{
		auto* subject = subjects.addSubject(name);
		auto* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);

		auto* spine = subject->addTransform("Spine", 0);
		spine->transformOrder.push_back(O3DS::TTranslation);
		spine->transformOrder.push_back(O3DS::TRotation);
	}

	bool NearlyEqual(double a, double b, double tol)
	{
		return std::abs(a - b) < tol;
	}

	// Establishes matching sender/receiver Subjects with the same rest-pose
	// anchor, exactly as a real sender/receiver pair would via a full
	// Serialize()/Parse() exchange.
	void FullSync(SubjectList& sender, SubjectList& receiver)
	{
		std::vector<char> buf;
		O3DS_CHECK(sender.Serialize(buf) > 0);
		O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));
	}
}

O3DS_TEST(D1_QuantizationDisabledByDefault_WireOutputHasNoQuantizedVectors)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver);

	Subject* s = sender.findSubject("Actor");
	s->mTransforms[0]->translation.value = Vector3d(0.001, 0.0, 0.0);

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count) > 0); // mQuantizationEnabled defaults false

	O3DS::SubjectList probe;
	O3DS_CHECK(probe.Parse(buf.data(), buf.size()));
	// No direct accessor for "did this update use quantized vectors" at the
	// SubjectList level - round-trip via the receiver instead and confirm
	// the value still applies correctly through the plain (non-quantized)
	// path, which is the behavior-level guarantee that matters here.
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));
	Subject* r = receiver.findSubject("Actor");
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], 0.001, 1.0e-9));
}

O3DS_TEST(D1_SmallTranslationMotion_RoundTripsViaByteTier)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver);

	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	// Small motion, well within byteRange, on the Root transform.
	s->mTransforms[0]->translation.value = Vector3d(0.005, -0.003, 0.002);

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	Subject* r = receiver.findSubject("Actor");
	// Byte tier resolution at range=0.01 is ~0.01/127 per axis.
	const double tol = (0.01 / 127.0) + 1.0e-9;
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], 0.005, tol));
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[1], -0.003, tol));
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[2], 0.002, tol));
}

O3DS_TEST(D1_MediumTranslationMotion_RoundTripsViaHalfTier)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver);

	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	s->mTransforms[0]->translation.value = Vector3d(0.5, -0.25, 0.1);

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	Subject* r = receiver.findSubject("Actor");
	const double tol = (1.0 / 32767.0) + 1.0e-9;
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], 0.5, tol));
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[1], -0.25, tol));
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[2], 0.1, tol));
}

O3DS_TEST(D1_LargeTranslationMotion_FallsBackToFullPrecision)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver);

	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	// Exceeds halfRange - must fall back to exact float32, not clamp/lose
	// precision the way a quantized tier would.
	s->mTransforms[0]->translation.value = Vector3d(123.456f, 0.0, 0.0);

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	Subject* r = receiver.findSubject("Actor");
	// Full float32 round-trip is exact modulo the double<->float cast
	// already inherent to the legacy (non-quantized) wire format.
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], (double)(float)123.456f, 1.0e-6));
}

O3DS_TEST(D1_RotationQuantization_RoundTripsViaByteAndHalfTiers)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver);

	sender.mQuantizationEnabled = true;
	// Rotation reuses byteRange/halfRange as a generic "how much did this
	// channel move" threshold against its own quaternion-space delta() -
	// small angle -> Byte, larger -> Half (the fresh-Subject delta() vs.
	// the identity-rotation default is what selects the tier here).
	sender.mQuantRanges.byteRange = 0.05;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	s->mTransforms[0]->rotation.value = QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), rad(3.0)); // small angle -> Byte tier
	s->mTransforms[1]->rotation.value = QuatFromAxisAngle(Vector3d(0.0, 1.0, 0.0), rad(60.0)); // larger -> Half tier

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	Subject* r = receiver.findSubject("Actor");
	auto angleDelta = [](const Quat& a, const Quat& b) {
		double dot = a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2] + a.v[3] * b.v[3];
		dot = std::max(-1.0, std::min(1.0, std::fabs(dot)));
		return 2.0 * std::acos(dot);
	};
	O3DS_CHECK(angleDelta(s->mTransforms[0]->rotation.value, r->mTransforms[0]->rotation.value) < rad(5.0));
	O3DS_CHECK(angleDelta(s->mTransforms[1]->rotation.value, r->mTransforms[1]->rotation.value) < rad(0.1));
}

O3DS_TEST(D1_TopologyResync_ReestablishesFreshAnchorOnBothSides)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver);

	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	s->mTransforms[0]->translation.value = Vector3d(50.0, 0.0, 0.0); // drive it far from the original anchor

	// A second full sync re-anchors both sides at this new value - a
	// subsequent SMALL delta from here should quantize via Byte tier again,
	// not be forced to Full (which it would be if the anchor were still the
	// original rest pose, since 50.0 - originalAnchor would exceed
	// halfRange).
	FullSync(sender, receiver);

	s->mTransforms[0]->translation.value = Vector3d(50.005, 0.0, 0.0);

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	Subject* r = receiver.findSubject("Actor");
	const double tol = (0.01 / 127.0) + 1.0e-9;
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], 50.005, tol));
}

O3DS_TEST(D1_NoAnchorYet_FallsBackToFullWithoutCrashing)
{
	// A Subject built directly (never gone through a full Serialize()) has
	// no rest-pose anchor - quantization must degrade gracefully to Full
	// rather than reading garbage or crashing.
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	s->mTransforms[0]->translation.value = Vector3d(0.001, 0.0, 0.0);

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);

	SubjectList receiver;
	BuildSkeleton(receiver, "Actor"); // receiver also has no anchor yet
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	Subject* r = receiver.findSubject("Actor");
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], 0.001, 1.0e-6));
}
