// SubjectList.scene_time (RCV-8, ADR 0013): the sender's engine timecode
// round-trips through every writer path (full, delta, quantized delta,
// residual; Subject and SubjectList; StreamWriter) and is read back by both
// PeekPacketMeta and SubjectList::Parse. Absent stays absent; an invalid value
// is never written and, when hand-built onto the wire, is read as absent while
// the frame is still applied; a frame without one never inherits the previous
// frame's; the field changes neither min_reader_version nor protocol_version.
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/predict/residual_codec.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/stream_writer.h"
#include "o3ds/wire_format.h"

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace O3DS;

namespace
{
	SceneTime MakeTime(int32_t frame, float subframe, int32_t num, int32_t den)
	{
		SceneTime t;
		t.frame = frame;
		t.subframe = subframe;
		t.rate_numerator = num;
		t.rate_denominator = den;
		return t;
	}

	bool Same(const SceneTime& a, const SceneTime& b)
	{
		return a.frame == b.frame && a.subframe == b.subframe
			&& a.rate_numerator == b.rate_numerator && a.rate_denominator == b.rate_denominator;
	}

	void BuildSkeleton(SubjectList& list)
	{
		Subject* subject = list.addSubject("Actor");
		Transform* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);
	}

	PacketMeta Peek(const std::vector<char>& frame)
	{
		PacketMeta meta;
		O3DS_CHECK(PeekPacketMeta(frame.data(), frame.size(), meta));
		return meta;
	}

	uint8_t MinReaderVersion(const std::vector<char>& frame)
	{
		return static_cast<uint8_t>(Wire::LoadLE32(frame.data()) & 0xFFu);
	}

	uint16_t WriterProtocol(const std::vector<char>& frame)
	{
		return O3DS::Data::GetSubjectList(frame.data() + Wire::kFrameHeaderSize)->protocol_version();
	}

	//! Checks that frame carries expected, through both readers.
	void ExpectSceneTime(const std::vector<char>& frame, const SceneTime& expected)
	{
		const PacketMeta meta = Peek(frame);
		O3DS_CHECK(meta.has_scene_time);
		O3DS_CHECK(Same(meta.scene_time, expected));
		SubjectList receiver;
		O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));
		O3DS_CHECK(receiver.mHasSceneTime);
		O3DS_CHECK(Same(receiver.mSceneTime, expected));
	}

	//! A full snapshot built by hand with a raw (possibly invalid) scene_time.
	std::vector<char> HandBuiltFrame(const O3DS::Data::SceneTime& raw)
	{
		flatbuffers::FlatBufferBuilder b;
		// One root node, so the subject itself is valid.
		const O3DS::Data::Translation translation(0.0f, 0.0f, 0.0f);
		const O3DS::Data::Rotation rotation(0.0f, 0.0f, 0.0f, 1.0f);
		const auto nodeName = b.CreateString("Root");
		std::vector<flatbuffers::Offset<O3DS::Data::Transform>> nodes;
		nodes.push_back(O3DS::Data::CreateTransform(b, -1, nodeName, &translation, &rotation));
		const auto nodeVector = b.CreateVector(nodes);
		std::vector<flatbuffers::Offset<O3DS::Data::Subject>> subjects;
		const auto name = b.CreateString("Actor");
		subjects.push_back(O3DS::Data::CreateSubject(b, nodeVector, name));
		const auto subjectVector = b.CreateVector(subjects);
		auto root = O3DS::Data::CreateSubjectList(b, subjectVector, 0, 1.0, 0, 0, 0, Wire::kProtocolVersion, &raw);
		std::vector<char> out;
		FinishSubjectListFrame(b, root, out);
		return out;
	}
}

O3DS_TEST(SceneTime_Validity)
{
	O3DS_CHECK(IsValidSceneTime(MakeTime(100, 0.0f, 24, 1)));
	O3DS_CHECK(IsValidSceneTime(MakeTime(-5, 0.5f, 30000, 1001)));
	O3DS_CHECK(!IsValidSceneTime(MakeTime(1, 0.0f, 0, 1)));
	O3DS_CHECK(!IsValidSceneTime(MakeTime(1, 0.0f, 24, 0)));
	O3DS_CHECK(!IsValidSceneTime(MakeTime(1, 0.0f, -24, 1)));
	O3DS_CHECK(!IsValidSceneTime(MakeTime(1, 1.0f, 24, 1)));
	O3DS_CHECK(!IsValidSceneTime(MakeTime(1, -0.25f, 24, 1)));
	O3DS_CHECK(!IsValidSceneTime(MakeTime(1, std::numeric_limits<float>::quiet_NaN(), 24, 1)));
	O3DS_CHECK(!IsValidSceneTime(MakeTime(1, std::numeric_limits<float>::infinity(), 24, 1)));
}

O3DS_TEST(SceneTime_RoundTripsThroughEveryWriterPath)
{
	SubjectList list;
	BuildSkeleton(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;
	const SceneTime t1 = MakeTime(86400, 0.25f, 24, 1);

	std::vector<char> full;
	O3DS_CHECK(writer.WriteFull(*subject, full, 1.0, &t1) > 0);
	ExpectSceneTime(full, t1);

	const SceneTime t2 = MakeTime(86401, 0.5f, 30000, 1001);
	subject->mTransforms[0]->translation.value = Vector3d(1.0, 0.0, 0.0);
	std::vector<char> update;
	size_t count = 0;
	O3DS_CHECK(writer.WriteUpdate(*subject, update, count, 1.0e-6, 1.02, nullptr, &t2) > 0);
	ExpectSceneTime(update, t2);

	const SceneTime t3 = MakeTime(86402, 0.0f, 60, 1);
	QuantRanges ranges;
	subject->mTransforms[0]->translation.value = Vector3d(1.001, 0.0, 0.0);
	std::vector<char> quantized;
	count = 0;
	O3DS_CHECK(writer.WriteUpdate(*subject, quantized, count, 1.0e-6, 1.04, &ranges, &t3) > 0);
	ExpectSceneTime(quantized, t3);

	const SceneTime t4 = MakeTime(86403, 0.75f, 50, 1);
	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	std::vector<char> residual;
	count = 0;
	O3DS_CHECK(writer.WriteResidual(*subject, residual, count, 1.0e-6, 1.06, &t4) > 0);
	ExpectSceneTime(residual, t4);

	const SceneTime t5 = MakeTime(86404, 0.125f, 25, 1);
	std::vector<char> listFull;
	O3DS_CHECK(writer.WriteFull(list, listFull, 1.08, &t5) > 0);
	ExpectSceneTime(listFull, t5);

	// The SubjectList update paths, through the serializers directly.
	SubjectList other;
	BuildSkeleton(other);
	std::vector<char> scratch;
	O3DS_CHECK(other.Serialize(scratch, 2.0) > 0);
	other.findSubject("Actor")->mTransforms[0]->translation.value = Vector3d(2.0, 0.0, 0.0);
	const SceneTime t6 = MakeTime(7, 0.0f, 24, 1);
	std::vector<char> listUpdate;
	count = 0;
	O3DS_CHECK(other.SerializeUpdate(listUpdate, count, 2.02, 10, 1, 1, &t6) > 0);
	ExpectSceneTime(listUpdate, t6);

	other.findSubject("Actor")->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	other.findSubject("Actor")->mTransforms[0]->translation.value = Vector3d(3.0, 0.0, 0.0);
	const SceneTime t7 = MakeTime(8, 0.0f, 24, 1);
	std::vector<char> listResidual;
	count = 0;
	O3DS_CHECK(other.SerializeUpdateResidual(listResidual, count, 2.04, 11, 1, 1, &t7) > 0);
	ExpectSceneTime(listResidual, t7);
}

O3DS_TEST(SceneTime_AbsentWhenNotGivenOrInvalid)
{
	SubjectList list;
	BuildSkeleton(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;

	std::vector<char> none;
	O3DS_CHECK(writer.WriteFull(*subject, none, 1.0) > 0);
	O3DS_CHECK(!Peek(none).has_scene_time);
	O3DS_CHECK(O3DS::Data::GetSubjectList(none.data() + Wire::kFrameHeaderSize)->scene_time() == nullptr);

	// The writer does not put an invalid timecode on the wire.
	const SceneTime invalid = MakeTime(1, 0.0f, 24, 0);
	std::vector<char> notWritten;
	O3DS_CHECK(writer.WriteFull(*subject, notWritten, 1.0, &invalid) > 0);
	O3DS_CHECK(O3DS::Data::GetSubjectList(notWritten.data() + Wire::kFrameHeaderSize)->scene_time() == nullptr);
	O3DS_CHECK(!Peek(notWritten).has_scene_time);
}

O3DS_TEST(SceneTime_InvalidOnTheWireIsAbsentAndTheFrameStillApplies)
{
	const O3DS::Data::SceneTime raws[] = {
		O3DS::Data::SceneTime(1, 0.0f, 24, 0),
		O3DS::Data::SceneTime(1, 0.0f, 0, 1),
		O3DS::Data::SceneTime(1, std::numeric_limits<float>::quiet_NaN(), 24, 1),
		O3DS::Data::SceneTime(1, 1.5f, 24, 1),
	};
	for (const O3DS::Data::SceneTime& raw : raws)
	{
		const std::vector<char> frame = HandBuiltFrame(raw);
		const PacketMeta meta = Peek(frame); // the frame is accepted
		O3DS_CHECK(!meta.has_scene_time);
		SubjectList receiver;
		O3DS_CHECK(receiver.Parse(frame.data(), frame.size()));
		O3DS_CHECK(!receiver.mHasSceneTime);
		O3DS_CHECK(receiver.findSubject("Actor") != nullptr);
	}
	// A valid hand-built one is read.
	const std::vector<char> valid = HandBuiltFrame(O3DS::Data::SceneTime(3, 0.5f, 24, 1));
	ExpectSceneTime(valid, MakeTime(3, 0.5f, 24, 1));
}

O3DS_TEST(SceneTime_AFrameWithoutOneDoesNotInheritThePrevious)
{
	SubjectList list;
	BuildSkeleton(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;
	const SceneTime t = MakeTime(10, 0.0f, 24, 1);
	std::vector<char> with;
	O3DS_CHECK(writer.WriteFull(*subject, with, 1.0, &t) > 0);
	std::vector<char> without;
	O3DS_CHECK(writer.WriteFull(*subject, without, 1.1) > 0);

	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(with.data(), with.size()));
	O3DS_CHECK(receiver.mHasSceneTime);
	O3DS_CHECK(receiver.Parse(without.data(), without.size()));
	O3DS_CHECK(!receiver.mHasSceneTime);
	// A rejected frame clears it too.
	O3DS_CHECK(receiver.Parse(with.data(), with.size()));
	std::vector<char> broken = with;
	broken[Wire::kFrameHeaderSize + 1] ^= 0x5A; // CRC mismatch
	O3DS_CHECK(!receiver.Parse(broken.data(), broken.size()));
	O3DS_CHECK(!receiver.mHasSceneTime);
}

O3DS_TEST(SceneTime_LeavesProtocolFieldsUnchanged)
{
	SubjectList list;
	BuildSkeleton(list);
	Subject* subject = list.findSubject("Actor");
	StreamWriter writer;
	const SceneTime t = MakeTime(10, 0.0f, 24, 1);

	std::vector<char> plain;
	O3DS_CHECK(writer.WriteFull(*subject, plain, 1.0) > 0);
	std::vector<char> timed;
	O3DS_CHECK(writer.WriteFull(*subject, timed, 1.0, &t) > 0);
	O3DS_CHECK(MinReaderVersion(timed) == MinReaderVersion(plain));
	O3DS_CHECK(MinReaderVersion(timed) == Wire::kMinReaderPlain);
	O3DS_CHECK(WriterProtocol(timed) == Wire::kProtocolVersion);
}
