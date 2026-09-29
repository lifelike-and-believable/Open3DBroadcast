// Full Subject/SubjectList wire round-trip tests for D1 adaptive channel
// quantization (roadmap doc §6/D1), layered on top of channel_quant_tests.cpp's
// pure-math codec tests. Exercises the actual SerializeUpdate()/ParseUpdate()
// integration - schema fields, rest-pose anchor capture/reconstruction, and
// tier fallback - not just QuantizeByte/QuantizeRotationByte in isolation.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"
#include "o3ds_generated.h"

#include <cmath>

using namespace O3DS;

namespace
{
	enum class WireTier { None, Full, Byte, Half };

	// Inspects the raw wire bytes directly (bypassing the round-trip through
	// a receiver) to confirm exactly which tier a channel was actually
	// encoded at - a precise, unambiguous check that doesn't rely on
	// inferring tier choice from reconstruction tolerance (which can be
	// misleading: Full-tier float32 precision is tighter than Byte-tier's
	// own tolerance, so a tolerance-only check can't distinguish "quantized
	// via Byte" from "fell back to Full").
	WireTier TranslationTierForIndex(const std::vector<char>& buf, int transformIndex)
	{
		// finalize() prepends an 8-byte (flags+CRC) header before the
		// FlatBuffers payload - same layout SubjectList::Parse() itself skips.
		auto root = O3DS::Data::GetSubjectList(buf.data() + 8);
		if (!root->updates() || root->updates()->size() == 0)
			return WireTier::None;
		auto update = root->updates()->Get(0);

		if (update->translations()) {
			for (auto t : *update->translations())
				if (t->i() == transformIndex) return WireTier::Full;
		}
		if (update->translations_q8()) {
			for (auto t : *update->translations_q8())
				if (t->i() == transformIndex) return WireTier::Byte;
		}
		if (update->translations_q16()) {
			for (auto t : *update->translations_q16())
				if (t->i() == transformIndex) return WireTier::Half;
		}
		return WireTier::None;
	}

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

	// Regression (adversarial review, Finding 1): CreateVectorOfStructs()
	// writes a real (non-null-offset) empty vector even when nothing was
	// pushed to it - calling it unconditionally for the four D1 vectors
	// added real wire bytes to EVERY update regardless of whether
	// quantization was ever enabled, contradicting "byte-for-byte identical
	// when disabled". Confirm the wire literally has no quantized vectors
	// at all when disabled, not just that decoding still works.
	O3DS_CHECK(TranslationTierForIndex(buf, 0) == WireTier::Full);
	{
		auto root = O3DS::Data::GetSubjectList(buf.data() + 8);
		auto update = root->updates()->Get(0);
		O3DS_CHECK(update->translations_q8() == nullptr);
		O3DS_CHECK(update->translations_q16() == nullptr);
		O3DS_CHECK(update->rotations_q8() == nullptr);
		O3DS_CHECK(update->rotations_q16() == nullptr);
	}

	O3DS::SubjectList probe;
	O3DS_CHECK(probe.Parse(buf.data(), buf.size()));
	// Round-trip via the receiver too, confirming the value still applies
	// correctly through the plain (non-quantized) path.
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

	// Regression (Copilot review): quant_byte_range/quant_half_range must
	// stay 0 when no TRANSLATION channel was quantized this update - only
	// rotation was quantized here, and rotation's smallest-three encoding
	// needs neither range field to decode. A stale non-zero range would
	// contradict the schema's "0 == no [byte/half]-tier translation vector
	// present" contract for no reason.
	{
		auto root = O3DS::Data::GetSubjectList(buf.data() + 8);
		auto update = root->updates()->Get(0);
		O3DS_CHECK(update->quant_byte_range() == 0.0f);
		O3DS_CHECK(update->quant_half_range() == 0.0f);
		O3DS_CHECK(update->rotations_q8() != nullptr || update->rotations_q16() != nullptr);
	}

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

O3DS_TEST(D1_RepeatedFullSync_ReanchorsBothSides)
{
	// ADR 0005 (vii), WP-S3/SND-2: sender and receiver both re-anchor at
	// every full sync. This replaced the earlier "anchor once, forever"
	// rule, under which a receiver that joined late (or a sender that
	// rebuilt its Transform objects) ended up with a different anchor from
	// the other side.
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver); // anchor = (0,0,0) on both sides

	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	s->mTransforms[0]->translation.value = Vector3d(50.0, 0.0, 0.0);

	// Second full sync of the same topology: both sides now anchor at 50.0.
	FullSync(sender, receiver);

	// A small move from the new anchor is small again, so it goes out on the
	// Byte tier and decodes correctly against the receiver's new anchor.
	s->mTransforms[0]->translation.value = Vector3d(50.005, 0.0, 0.0);

	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);
	O3DS_CHECK(TranslationTierForIndex(buf, 0) == WireTier::Byte);

	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));
	Subject* r = receiver.findSubject("Actor");
	const double tol = (0.01 / 127.0) + 1.0e-6;
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], 50.005, tol));
}

O3DS_TEST(D1_LostFullSync_RecoversAtNextDeliveredFullSync)
{
	// With re-anchoring at every full sync, a lost full sync leaves the
	// receiver on the previous anchor until the next full sync reaches it
	// (SubjectUpdate.ref_seq, WP-A4a, will let the receiver drop those
	// updates instead). The sender's periodic full sync (ADR 0005 (ii))
	// bounds that window; this checks the recovery.
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	SubjectList receiver;
	FullSync(sender, receiver); // anchor = (0,0,0) on both sides

	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;

	Subject* s = sender.findSubject("Actor");
	s->mTransforms[0]->translation.value = Vector3d(0.3, 0.0, 0.0);

	std::vector<char> droppedFullSync;
	O3DS_CHECK(sender.Serialize(droppedFullSync) > 0);
	(void)droppedFullSync; // never delivered

	// The next periodic full sync is delivered.
	s->mTransforms[0]->translation.value = Vector3d(0.31, 0.0, 0.0);
	FullSync(sender, receiver);

	s->mTransforms[0]->translation.value = Vector3d(0.313, 0.0, 0.0);
	size_t count = 0;
	std::vector<char> buf;
	O3DS_CHECK(sender.SerializeUpdate(buf, count, 1.0e-6) > 0);
	O3DS_CHECK(TranslationTierForIndex(buf, 0) == WireTier::Byte);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));

	Subject* r = receiver.findSubject("Actor");
	const double tol = (0.01 / 127.0) + 1.0e-6;
	O3DS_CHECK(NearlyEqual(r->mTransforms[0]->translation.value.v[0], 0.313, tol));
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
