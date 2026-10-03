// ADR 0005 (viii)/(ix) resync contract (CORE-5, CORE-6): every update names
// the full Subject it is relative to (SubjectUpdate.ref_seq), and a receiver
// given a ParseContext drops updates it cannot apply correctly - one relative
// to a full Subject it did not apply, a residual update after a sequence gap,
// or a residual update its decoder has no history for - until the next full
// Subject resynchronises both ends.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/stream_writer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace O3DS;

namespace
{
	void BuildActor(SubjectList& list)
	{
		Subject* subject = list.addSubject("Actor");
		subject->mCurveNames = { "Smile" };
		subject->mCurveValues = { 0.0f };
		Transform* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);
		Transform* head = subject->addTransform("Head", 0);
		head->transformOrder.push_back(O3DS::TTranslation);
		head->transformOrder.push_back(O3DS::TRotation);
	}

	void Move(Subject* subject, double t)
	{
		subject->mTransforms[0]->translation.value = Vector3d(1.0 * t, 0.5 * t, 0.0);
		subject->mTransforms[0]->rotation.value = QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), 0.4 * t);
		subject->mTransforms[1]->translation.value = Vector3d(0.0, 1.0 + 0.2 * t, -0.3 * t);
		subject->mTransforms[1]->rotation.value = QuatFromAxisAngle(Vector3d(1.0, 0.0, 0.0), -0.7 * t);
		subject->mCurveValues[0] = (float)std::sin(t);
	}

	bool SamePose(Subject* a, Subject* b, double tolerance)
	{
		if (a == nullptr || b == nullptr || a->mTransforms.size() != b->mTransforms.size())
			return false;
		for (size_t n = 0; n < a->mTransforms.size(); ++n)
		{
			for (int k = 0; k < 3; ++k)
			{
				if (std::abs(a->mTransforms[n]->translation.value.v[k] - b->mTransforms[n]->translation.value.v[k]) > tolerance)
					return false;
			}
			for (int k = 0; k < 4; ++k)
			{
				if (std::abs(a->mTransforms[n]->rotation.value.v[k] - b->mTransforms[n]->rotation.value.v[k]) > tolerance)
					return false;
			}
		}
		return std::abs(a->mCurveValues[0] - b->mCurveValues[0]) <= tolerance;
	}

	uint64_t RefSeqOf(const std::vector<char>& frame)
	{
		const auto* list = O3DS::Data::GetSubjectList(frame.data() + 8);
		return (list->updates() && list->updates()->size() == 1) ? list->updates()->Get(0)->ref_seq() : ~0ull;
	}

	//! A receiver stream: parses with the context a gated receiver would pass.
	struct Receiver
	{
		ReceiverStream stream;
		std::vector<ParsedSubjectInfo> touched;

		bool Apply(const std::vector<char>& frame)
		{
			PacketMeta meta;
			if (!PeekPacketMeta(frame.data(), frame.size(), meta))
				return false;
			const ParseContext context = MakeParseContext(stream, meta.tx_seq, meta.frame_epoch);
			if (!stream.subjects.Parse(frame.data(), frame.size(), nullptr, true, &touched, &context))
				return false;
			NoteFrameApplied(stream, meta.tx_seq, meta.frame_epoch);
			return true;
		}

		bool Touched() const { return !touched.empty(); }
		Subject* Actor() { return stream.subjects.findSubject("Actor"); }
		uint64_t Dropped() const { return stream.subjects.mUpdatesDroppedUnsynced; }
	};
}

O3DS_TEST(Resync_UpdatesCarryTheLastFullSeqAsRef)
{
	SubjectList list;
	BuildActor(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;

	std::vector<char> frame;
	size_t count = 0;
	O3DS_CHECK(writer.LastFullSeq("Actor") == 0);
	Move(subject, 0.0);
	O3DS_CHECK(writer.WriteUpdate(*subject, frame, count, 0.0, 0.0) > 0);
	O3DS_CHECK(RefSeqOf(frame) == 0); // no full Subject yet: unset

	O3DS_CHECK(writer.WriteFull(*subject, frame, 0.02) > 0); // seq 2
	O3DS_CHECK(writer.LastFullSeq("Actor") == 2);
	Move(subject, 0.04);
	O3DS_CHECK(writer.WriteUpdate(*subject, frame, count, 0.0, 0.04) > 0);
	O3DS_CHECK(RefSeqOf(frame) == 2);

	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	O3DS_CHECK(writer.WriteFull(*subject, frame, 0.06) > 0); // seq 4
	Move(subject, 0.08);
	O3DS_CHECK(writer.WriteResidual(*subject, frame, count, 0.0, 0.08) > 0);
	O3DS_CHECK(RefSeqOf(frame) == 4);
}

O3DS_TEST(Resync_UpdateRelativeToAMissedFullSubjectIsDropped)
{
	SubjectList list;
	BuildActor(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;
	Receiver rx;

	std::vector<char> frame;
	size_t count = 0;
	Move(subject, 0.0);
	O3DS_CHECK(writer.WriteFull(*subject, frame, 0.0) > 0); // seq 1
	O3DS_CHECK(rx.Apply(frame) && rx.Touched());
	Move(subject, 0.02);
	O3DS_CHECK(writer.WriteUpdate(*subject, frame, count, 0.0, 0.02) > 0); // seq 2, ref 1
	O3DS_CHECK(rx.Apply(frame) && rx.Touched());
	O3DS_CHECK(SamePose(rx.Actor(), subject, 1.0e-5));

	// The next full Subject (seq 3) is lost; its updates name it.
	O3DS_CHECK(writer.WriteFull(*subject, frame, 0.04) > 0);
	for (int i = 0; i < 3; ++i)
	{
		Move(subject, 0.06 + 0.02 * i);
		O3DS_CHECK(writer.WriteUpdate(*subject, frame, count, 0.0, 0.06 + 0.02 * i) > 0);
		O3DS_CHECK(RefSeqOf(frame) == 3);
		// Not an error: the frame parses, but the update is dropped and not reported.
		O3DS_CHECK(rx.Apply(frame));
		O3DS_CHECK(!rx.Touched());
	}
	O3DS_CHECK(rx.Dropped() == 3);

	// The next full Subject resynchronises.
	Move(subject, 0.12);
	O3DS_CHECK(writer.WriteFull(*subject, frame, 0.12) > 0);
	O3DS_CHECK(rx.Apply(frame) && rx.Touched());
	Move(subject, 0.14);
	O3DS_CHECK(writer.WriteUpdate(*subject, frame, count, 0.0, 0.14) > 0);
	O3DS_CHECK(rx.Apply(frame) && rx.Touched());
	O3DS_CHECK(SamePose(rx.Actor(), subject, 1.0e-5));
	O3DS_CHECK(rx.Dropped() == 3);
}

O3DS_TEST(Resync_ResidualGapHoldsUntilTheNextFullSubjectThenMatchesTheSender)
{
	SubjectList list;
	BuildActor(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;
	Receiver rx;

	// As the UE serializer does: each full sync gets a fresh encoder.
	auto writeFull = [&](double t)
	{
		std::vector<char> frame;
		Move(subject, t);
		subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear, /*keyframeIntervalFrames*/ 4));
		O3DS_CHECK(writer.WriteFull(*subject, frame, t) > 0);
		return frame;
	};
	auto writeResidual = [&](double t)
	{
		std::vector<char> frame;
		size_t count = 0;
		Move(subject, t);
		O3DS_CHECK(writer.WriteResidual(*subject, frame, count, 0.0, t) > 0);
		return frame;
	};

	double t = 0.0;
	O3DS_CHECK(rx.Apply(writeFull(t)));
	for (int i = 0; i < 6; ++i)
	{
		t += 0.02;
		O3DS_CHECK(rx.Apply(writeResidual(t)) && rx.Touched());
		O3DS_CHECK(SamePose(rx.Actor(), subject, 1.0e-4));
	}

	// One residual update is lost. Everything after it, including the
	// cadence keyframes (every 4 frames), waits for the next full Subject: a
	// residual keyframe does not reset predictor history on either end, so
	// decoding from it would not match the sender.
	t += 0.02;
	writeResidual(t);
	const uint64_t droppedBefore = rx.Dropped();
	for (int i = 0; i < 10; ++i)
	{
		t += 0.02;
		O3DS_CHECK(rx.Apply(writeResidual(t)));
		O3DS_CHECK(!rx.Touched());
	}
	O3DS_CHECK(rx.Dropped() == droppedBefore + 10);

	// The full Subject resynchronises, and every later frame matches.
	t += 0.02;
	O3DS_CHECK(rx.Apply(writeFull(t)) && rx.Touched());
	O3DS_CHECK(SamePose(rx.Actor(), subject, 1.0e-4));
	for (int i = 0; i < 20; ++i)
	{
		t += 0.02;
		O3DS_CHECK(rx.Apply(writeResidual(t)) && rx.Touched());
		O3DS_CHECK(SamePose(rx.Actor(), subject, 1.0e-4));
	}
	O3DS_CHECK(rx.Dropped() == droppedBefore + 10);
}

O3DS_TEST(Resync_ANewEpochInvalidatesTheOldFullSubject)
{
	SubjectList list;
	BuildActor(list);
	Subject* subject = list.findSubject("Actor");
	Receiver rx;

	std::vector<char> frame;
	size_t count = 0;
	Move(subject, 0.0);
	O3DS_CHECK(subject->Serialize(frame, 1.0, /*tx_seq*/ 1, /*wallclock*/ 1, /*epoch*/ 100) > 0);
	O3DS_CHECK(rx.Apply(frame) && rx.Touched());

	// A restarted sender (epoch 101) whose update names seq 1 of its own
	// session: the receiver applied seq 1 of epoch 100, not this one.
	Move(subject, 0.02);
	O3DS_CHECK(subject->SerializeUpdate(frame, count, 0.0, 1.02, nullptr, /*tx_seq*/ 2, /*wallclock*/ 2, /*epoch*/ 101, /*ref_seq*/ 1) > 0);
	O3DS_CHECK(rx.Apply(frame));
	O3DS_CHECK(!rx.Touched());
	O3DS_CHECK(rx.Dropped() == 1);
}

O3DS_TEST(Resync_UnsetRefSeqAndUnsequencedFramesApplyAsBefore)
{
	SubjectList list;
	BuildActor(list);
	Subject* subject = list.findSubject("Actor");

	std::vector<char> full;
	Move(subject, 0.0);
	O3DS_CHECK(subject->Serialize(full, 1.0, 1, 1, 7) > 0);

	// An update naming a full Subject the receiver never saw...
	std::vector<char> update;
	size_t count = 0;
	Move(subject, 0.02);
	O3DS_CHECK(subject->SerializeUpdate(update, count, 0.0, 1.02, nullptr, 3, 3, 7, /*ref_seq*/ 2) > 0);

	// ...applies without a context, as before ADR 0005.
	SubjectList plain;
	std::vector<ParsedSubjectInfo> touched;
	O3DS_CHECK(plain.Parse(full.data(), full.size()));
	O3DS_CHECK(plain.Parse(update.data(), update.size(), nullptr, true, &touched));
	O3DS_CHECK(touched.size() == 1 && plain.mUpdatesDroppedUnsynced == 0);

	// With a context, an update whose ref_seq is unset (0) also applies.
	std::vector<char> unsetRef;
	O3DS_CHECK(subject->SerializeUpdate(unsetRef, count, 0.0, 1.04, nullptr, 4, 4, 7, /*ref_seq*/ 0) > 0);
	Receiver rx;
	O3DS_CHECK(rx.Apply(full) && rx.Apply(unsetRef) && rx.Touched());
	O3DS_CHECK(rx.Dropped() == 0);
}

O3DS_TEST(Resync_ResidualUpdateWithoutDecoderHistoryIsDropped)
{
	// CORE-6, without any context: the receiver missed the first residual
	// updates after the full Subject (the keyframes that build history), so
	// the next one cannot be decoded. Before, it was decoded against a zero
	// reference.
	SubjectList list;
	BuildActor(list);
	Subject* subject = list.findSubject("Actor");
	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));

	std::vector<char> frame;
	Move(subject, 0.0);
	O3DS_CHECK(subject->Serialize(frame, 0.0) > 0);
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));

	size_t count = 0;
	for (int i = 1; i <= 2; ++i) // keyframes while the encoder builds history; lost
	{
		Move(subject, 0.02 * i);
		O3DS_CHECK(subject->SerializeUpdateResidual(frame, count, 0.0, 0.02 * i) > 0);
	}
	Move(subject, 0.06);
	O3DS_CHECK(subject->SerializeUpdateResidual(frame, count, 0.0, 0.06) > 0);
	O3DS_CHECK(!O3DS::Data::GetSubjectList(frame.data() + 8)->updates()->Get(0)->is_keyframe());

	std::vector<ParsedSubjectInfo> touched;
	O3DS_CHECK(receiver.Parse(frame.data(), frame.size(), nullptr, true, &touched));
	O3DS_CHECK(touched.empty());
	O3DS_CHECK(receiver.mUpdatesDroppedUnsynced == 1);
	O3DS_CHECK(receiver.mError.empty());
}

O3DS_TEST(Resync_MakeParseContextFlagsGapsWithinAnEpoch)
{
	ReceiverStream stream;
	O3DS_CHECK(!MakeParseContext(stream, 5, 1).gap_before); // first frame: nothing to compare
	NoteFrameApplied(stream, 5, 1);
	O3DS_CHECK(!MakeParseContext(stream, 6, 1).gap_before);
	O3DS_CHECK(MakeParseContext(stream, 7, 1).gap_before);
	O3DS_CHECK(!MakeParseContext(stream, 1, 2).gap_before); // a new epoch starts afresh
	O3DS_CHECK(!MakeParseContext(stream, 0, 0).gap_before); // unsequenced
	NoteFrameApplied(stream, 0, 0);                           // ignored
	O3DS_CHECK(!MakeParseContext(stream, 6, 1).gap_before);
}
