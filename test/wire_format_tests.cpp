// D8 protocol versioning (docs/adr/0009-protocol-versioning.md): the frame
// word is the minimum reader protocol version, writers stamp it from what a
// frame contains, version-2 frames carry the "O3DS" file identifier and
// SubjectList.protocol_version, and readers (Parse, PeekMeta, PeekPacketMeta)
// accept 1..O3DS_PROTOCOL_VERSION and nothing else.
#include "test_framework.h"

#include "o3ds/crc32.h"
#include "o3ds/model.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/wire_format.h"
#include "o3ds/predict/residual_codec.h"
#include "o3ds_generated.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <memory>
#include <vector>

using namespace O3DS;

namespace
{
	void BuildSkeleton(SubjectList& list, const std::string& name)
	{
		Subject* subject = list.addSubject(name);
		Transform* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);
		Transform* spine = subject->addTransform("Spine", 0);
		spine->transformOrder.push_back(O3DS::TTranslation);
		spine->transformOrder.push_back(O3DS::TRotation);
	}

	uint8_t FrameMinReader(const std::vector<char>& frame)
	{
		return static_cast<uint8_t>(Wire::LoadLE32(frame.data()) & 0xFFu);
	}

	bool HasIdentifier(const std::vector<char>& frame)
	{
		return O3DS::Data::SubjectListBufferHasIdentifier(frame.data() + Wire::kFrameHeaderSize);
	}

	uint16_t WriterProtocol(const std::vector<char>& frame)
	{
		return O3DS::Data::GetSubjectList(frame.data() + Wire::kFrameHeaderSize)->protocol_version();
	}

	//! A full snapshot built by hand, the way a pre-D8 writer did: no
	//! identifier, no protocol_version, frame word as given.
	std::vector<char> HandBuiltFullFrame(uint32_t frameWord, bool withIdentifier)
	{
		flatbuffers::FlatBufferBuilder b;
		std::vector<flatbuffers::Offset<O3DS::Data::Subject>> subjects;
		const auto name = b.CreateString("Actor");
		subjects.push_back(O3DS::Data::CreateSubject(b, 0, name));
		auto root = O3DS::Data::CreateSubjectListDirect(b, &subjects, nullptr, 1.0);
		if (withIdentifier)
			O3DS::Data::FinishSubjectListBuffer(b, root);
		else
			b.Finish(root);
		std::vector<char> out;
		finalize(b, out, frameWord);
		return out;
	}

	//! An update with a quantized vector, stamped with the given frame word.
	std::vector<char> HandBuiltQuantizedFrame(uint32_t frameWord, bool withIdentifier)
	{
		flatbuffers::FlatBufferBuilder b;
		std::vector<O3DS::Data::TranslationUpdateQ8> q8;
		q8.push_back(O3DS::Data::TranslationUpdateQ8(1, 2, 3, 0));
		std::vector<flatbuffers::Offset<O3DS::Data::SubjectUpdate>> updates;
		// Strings and vectors before the table builder starts.
		const auto name = b.CreateString("Actor");
		const auto q8vec = b.CreateVectorOfStructs(q8);
		O3DS::Data::SubjectUpdateBuilder ub(b);
		ub.add_name(name);
		ub.add_translations_q8(q8vec);
		updates.push_back(ub.Finish());
		auto root = O3DS::Data::CreateSubjectListDirect(b, nullptr, &updates, 1.0);
		if (withIdentifier)
			O3DS::Data::FinishSubjectListBuffer(b, root);
		else
			b.Finish(root);
		std::vector<char> out;
		finalize(b, out, frameWord);
		return out;
	}

	//! A real full snapshot rewritten into what a pre-D8 writer sent: frame
	//! word 1 and no file identifier (FlatBuffers leaves bytes 4-7 unused
	//! without one), CRC recomputed.
	std::vector<char> PreD8FullFrame()
	{
		SubjectList list;
		BuildSkeleton(list, "Actor");
		std::vector<char> frame;
		list.Serialize(frame, 1.0);
		std::memset(frame.data() + Wire::kFrameHeaderSize + 4, 0, 4);
		Wire::StoreLE32(frame.data(), 1);
		Wire::StoreLE32(frame.data() + 4, Crc32(frame.data() + Wire::kFrameHeaderSize, frame.size() - Wire::kFrameHeaderSize));
		return frame;
	}

	Wire::FrameCheck Check(const std::vector<char>& frame, uint8_t& minReader)
	{
		return CheckFrame(frame.data(), frame.size(), minReader);
	}
}

O3DS_TEST(Wire_LittleEndianHelpers)
{
	unsigned char bytes[4] = {};
	Wire::StoreLE32(bytes, 0x04030201u);
	O3DS_CHECK(bytes[0] == 1 && bytes[1] == 2 && bytes[2] == 3 && bytes[3] == 4);
	O3DS_CHECK(Wire::LoadLE32(bytes) == 0x04030201u);
	// Version 1 is the pre-D8 flags word, byte for byte.
	O3DS_CHECK(Wire::MakeFrameWord(1) == 0x00000001u);
}

O3DS_TEST(Wire_PlainWritersStampVersionOneWithIdentifier)
{
	SubjectList list;
	BuildSkeleton(list, "Actor");

	std::vector<char> full;
	O3DS_CHECK(list.Serialize(full, 1.0) > 8);
	O3DS_CHECK(FrameMinReader(full) == 1);
	O3DS_CHECK(HasIdentifier(full));
	O3DS_CHECK(WriterProtocol(full) == O3DS_PROTOCOL_VERSION);

	list.findSubject("Actor")->mTransforms[0]->translation.value = Vector3d(1.0, 0.0, 0.0);
	size_t count = 0;
	std::vector<char> delta;
	O3DS_CHECK(list.SerializeUpdate(delta, count, 1.5) > 8);
	O3DS_CHECK(FrameMinReader(delta) == 1);
	O3DS_CHECK(HasIdentifier(delta));

	std::vector<char> single;
	O3DS_CHECK(list.findSubject("Actor")->Serialize(single, 2.0) > 8);
	O3DS_CHECK(FrameMinReader(single) == 1);
	O3DS_CHECK(WriterProtocol(single) == O3DS_PROTOCOL_VERSION);
}

O3DS_TEST(Wire_QuantizedWriterStampsVersionTwoOnlyWhenItQuantizes)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	std::vector<char> full;
	O3DS_CHECK(sender.Serialize(full, 1.0) > 8);

	sender.mQuantizationEnabled = true;
	sender.mQuantRanges.byteRange = 0.01;
	sender.mQuantRanges.halfRange = 1.0;
	sender.findSubject("Actor")->mTransforms[0]->translation.value = Vector3d(0.005, -0.003, 0.002);

	size_t count = 0;
	std::vector<char> quantized;
	O3DS_CHECK(sender.SerializeUpdate(quantized, count, 1.0e-6) > 8);
	const O3DS::Data::SubjectUpdate* update = O3DS::Data::GetSubjectList(quantized.data() + 8)->updates()->Get(0);
	O3DS_CHECK(update->translations_q8() != nullptr);
	O3DS_CHECK(FrameMinReader(quantized) == Wire::kMinReaderResidualOrQuantized);
	O3DS_CHECK(HasIdentifier(quantized));

	// A new reader applies it.
	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(full.data(), full.size()));
	O3DS_CHECK(receiver.Parse(quantized.data(), quantized.size()));

	// Quantization on, but nothing moved: no quantized vector, so version 1.
	std::vector<char> idle;
	count = 0;
	O3DS_CHECK(sender.SerializeUpdate(idle, count, 1.0e-6) > 8);
	const O3DS::Data::SubjectUpdate* idleUpdate = O3DS::Data::GetSubjectList(idle.data() + 8)->updates()->Get(0);
	const bool hasQuantized = idleUpdate->translations_q8() || idleUpdate->translations_q16()
		|| idleUpdate->rotations_q8() || idleUpdate->rotations_q16() || idleUpdate->curves_q8() || idleUpdate->curves_q16();
	O3DS_CHECK(FrameMinReader(idle) == (hasQuantized ? 2 : 1));
}

O3DS_TEST(Wire_ResidualWriterStampsVersionTwo)
{
	SubjectList sender;
	BuildSkeleton(sender, "Actor");
	Subject* subject = sender.findSubject("Actor");
	subject->SetResidualEncoder(std::make_unique<ResidualEncoder>(ResidualPredictorId::Linear));
	std::vector<char> full;
	O3DS_CHECK(sender.Serialize(full, 1.0) > 8);

	subject->mTransforms[0]->translation.value = Vector3d(0.5, 0.0, 0.0);
	size_t count = 0;
	std::vector<char> residual;
	O3DS_CHECK(sender.SerializeUpdateResidual(residual, count, 1.02) > 8);
	O3DS_CHECK(FrameMinReader(residual) == Wire::kMinReaderResidualOrQuantized);
	O3DS_CHECK(WriterProtocol(residual) == O3DS_PROTOCOL_VERSION);

	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(full.data(), full.size()));
	receiver.findSubject("Actor")->SetResidualDecoder(std::make_unique<ResidualDecoder>(ResidualPredictorId::Linear));
	O3DS_CHECK(receiver.Parse(residual.data(), residual.size()));
}

O3DS_TEST(Wire_ReaderAcceptsPreD8PlainFrames)
{
	// What every pre-D8 writer sends: frame word 1, no identifier.
	const std::vector<char> legacy = PreD8FullFrame();
	O3DS_CHECK(!HasIdentifier(legacy));
	uint8_t minReader = 0;
	O3DS_CHECK(Check(legacy, minReader) == Wire::FrameCheck::Ok);
	O3DS_CHECK(minReader == 1);
	SubjectList list;
	O3DS_CHECK(list.Parse(legacy.data(), legacy.size()));
	O3DS_CHECK(list.findSubject("Actor") != nullptr);
	O3DS_CHECK(list.mLastFrameCheck == Wire::FrameCheck::Ok);

	uint64_t seq = 0, wallclock = 0;
	uint32_t epoch = 0;
	O3DS_CHECK(SubjectList::PeekMeta(legacy.data(), legacy.size(), seq, wallclock, epoch));
	PacketMeta meta;
	O3DS_CHECK(PeekPacketMeta(legacy.data(), legacy.size(), meta));
	O3DS_CHECK(meta.min_reader_version == 1);
}

O3DS_TEST(Wire_ReaderRejectsNewerProtocols)
{
	std::vector<char> future = HandBuiltFullFrame(Wire::MakeFrameWord(O3DS_PROTOCOL_VERSION + 1), true);
	uint8_t minReader = 0;
	O3DS_CHECK(Check(future, minReader) == Wire::FrameCheck::VersionTooNew);
	O3DS_CHECK(minReader == O3DS_PROTOCOL_VERSION + 1);

	SubjectList list;
	O3DS_CHECK(!list.Parse(future.data(), future.size()));
	O3DS_CHECK(list.mLastFrameCheck == Wire::FrameCheck::VersionTooNew);
	O3DS_CHECK(list.mLastFrameMinReaderVersion == O3DS_PROTOCOL_VERSION + 1);
	O3DS_CHECK(list.mError.find("requires protocol 3") != std::string::npos);

	PacketMeta meta;
	O3DS_CHECK(!PeekPacketMeta(future.data(), future.size(), meta));
	O3DS_CHECK(meta.check == Wire::FrameCheck::VersionTooNew);
	O3DS_CHECK(meta.min_reader_version == O3DS_PROTOCOL_VERSION + 1);
	uint64_t seq = 0, wallclock = 0;
	uint32_t epoch = 0;
	O3DS_CHECK(!SubjectList::PeekMeta(future.data(), future.size(), seq, wallclock, epoch));
}

O3DS_TEST(Wire_ReaderRejectsBadFrameWords)
{
	uint8_t minReader = 0;
	O3DS_CHECK(Check(HandBuiltFullFrame(0, false), minReader) == Wire::FrameCheck::BadFrameWord);
	// A flag or reserved byte set.
	O3DS_CHECK(Check(HandBuiltFullFrame(0x00000101u, false), minReader) == Wire::FrameCheck::BadFrameWord);
	O3DS_CHECK(Check(HandBuiltFullFrame(0x01000001u, false), minReader) == Wire::FrameCheck::BadFrameWord);
	// A pre-D8 big-endian-looking word.
	O3DS_CHECK(Check(HandBuiltFullFrame(0x01000000u, false), minReader) == Wire::FrameCheck::BadFrameWord);

	const char shortBuf[4] = { 1, 0, 0, 0 };
	O3DS_CHECK(CheckFrame(shortBuf, sizeof(shortBuf), minReader) == Wire::FrameCheck::TooShort);
	O3DS_CHECK(CheckFrame(nullptr, 0, minReader) == Wire::FrameCheck::TooShort);
}

O3DS_TEST(Wire_VersionTwoFramesNeedTheIdentifier)
{
	uint8_t minReader = 0;
	O3DS_CHECK(Check(HandBuiltQuantizedFrame(2, true), minReader) == Wire::FrameCheck::Ok);
	O3DS_CHECK(Check(HandBuiltQuantizedFrame(2, false), minReader) == Wire::FrameCheck::VerifyFailed);
	// A version-1 frame may carry the identifier (a D8 writer's plain frame).
	O3DS_CHECK(Check(HandBuiltFullFrame(1, true), minReader) == Wire::FrameCheck::Ok);
}

O3DS_TEST(Wire_ReaderRejectsUndeclaredResidualOrQuantizedContent)
{
	// Pre-D8 develop writers stamped 1 on quantized frames (ADR 0009 item 2).
	const std::vector<char> legacyQuantized = HandBuiltQuantizedFrame(1, false);
	uint8_t minReader = 0;
	O3DS_CHECK(Check(legacyQuantized, minReader) == Wire::FrameCheck::UndeclaredNewContent);
	SubjectList list;
	O3DS_CHECK(!list.Parse(legacyQuantized.data(), legacyQuantized.size()));
	O3DS_CHECK(list.mLastFrameCheck == Wire::FrameCheck::UndeclaredNewContent);
}

O3DS_TEST(Wire_PeekChecksTheCrc)
{
	// CORE-15: PeekMeta and PeekPacketMeta used to skip the CRC.
	SubjectList list;
	BuildSkeleton(list, "Actor");
	std::vector<char> frame;
	O3DS_CHECK(list.Serialize(frame, 1.0, 7, 0, 1) > 8);
	frame[5] = static_cast<char>(frame[5] ^ 0x5A);

	uint64_t seq = 0, wallclock = 0;
	uint32_t epoch = 0;
	O3DS_CHECK(!SubjectList::PeekMeta(frame.data(), frame.size(), seq, wallclock, epoch));
	PacketMeta meta;
	O3DS_CHECK(!PeekPacketMeta(frame.data(), frame.size(), meta));
	O3DS_CHECK(meta.check == Wire::FrameCheck::CrcMismatch);
}

namespace
{
	std::vector<char> ReadFixture(const char* name)
	{
		std::ifstream in(std::string(O3DS_WIRE_FIXTURE_DIR) + "/v1/" + name, std::ios::binary);
		return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
}

O3DS_TEST(Wire_BaselineWriterFramesStillParse)
{
	// ADR 0009 item 11, "baseline, new": plain frames parse as before; the
	// baseline's quantized and residual frames (stamped 1) are rejected.
	const std::vector<char> full = ReadFixture("full.o3ds");
	const std::vector<char> delta = ReadFixture("delta.o3ds");
	O3DS_CHECK(full.size() > Wire::kFrameHeaderSize && delta.size() > Wire::kFrameHeaderSize);
	O3DS_CHECK(!HasIdentifier(full));

	SubjectList list;
	O3DS_CHECK(list.Parse(full.data(), full.size()));
	Subject* actor = list.findSubject("Actor");
	O3DS_CHECK(actor != nullptr && actor->mTransforms.size() == 2);
	O3DS_CHECK(list.Parse(delta.data(), delta.size()));
	O3DS_CHECK(actor != nullptr && actor->mTransforms[0]->translation.value.v[0] == 0.25);

	for (const char* name : { "quantized.o3ds", "residual.o3ds" })
	{
		const std::vector<char> frame = ReadFixture(name);
		O3DS_CHECK(frame.size() > Wire::kFrameHeaderSize);
		SubjectList reader;
		O3DS_CHECK(reader.Parse(full.data(), full.size()));
		O3DS_CHECK(!reader.Parse(frame.data(), frame.size()));
		O3DS_CHECK(reader.mLastFrameCheck == Wire::FrameCheck::UndeclaredNewContent);
	}
}

O3DS_TEST(Envelope_V2LayoutIsLittleEndianWithSeq)
{
	unsigned char b[Wire::kEnvelopeV2HeaderSize + 3] = {};
	Wire::WriteEnvelopeHeaderV2(b, Wire::EnvelopeKind::Audio, Wire::EnvelopeCodec::PCM16, 0x0807060504030201ull, 3, 0x0D0C0B0Au);
	const unsigned char expected[24] = { 'O', '3', 'D', 'U', 2, 1, 2, 0,
		1, 2, 3, 4, 5, 6, 7, 8,  3, 0, 0, 0,  0x0A, 0x0B, 0x0C, 0x0D };
	O3DS_CHECK(std::memcmp(b, expected, sizeof(expected)) == 0);

	Wire::EnvelopeHeader h;
	O3DS_CHECK(Wire::ReadEnvelopeHeader(b, sizeof(b), h));
	O3DS_CHECK(h.version == 2 && h.kind == 1 && h.codec == 2 && h.flags == 0);
	O3DS_CHECK(h.timestamp_us == 0x0807060504030201ull);
	O3DS_CHECK(h.payload_size == 3 && h.seq == 0x0D0C0B0Au && h.header_size == 24);
	O3DS_CHECK(Wire::HasEnvelopeMagic(b, sizeof(b)));

	// The payload must fit; trailing bytes are allowed.
	O3DS_CHECK(!Wire::ReadEnvelopeHeader(b, Wire::kEnvelopeV2HeaderSize + 2, h));
	// Version 2 only, flags 0.
	b[4] = 3;
	O3DS_CHECK(!Wire::ReadEnvelopeHeader(b, sizeof(b), h));
	b[4] = 2;
	b[7] = 1;
	O3DS_CHECK(!Wire::ReadEnvelopeHeader(b, sizeof(b), h));
}

O3DS_TEST(Envelope_V1IsStillReadBigEndian)
{
	// The pre-D8 layout, as deployed writers send it.
	const unsigned char v1[20 + 2] = { 'O', '3', 'D', 'A', 1, 2, 3, 0,
		0, 0, 0, 0, 0, 0x0F, 0x42, 0x40,  0, 0, 0, 2,  0xAA, 0xBB };
	Wire::EnvelopeHeader h;
	O3DS_CHECK(Wire::ReadEnvelopeHeader(v1, sizeof(v1), h));
	O3DS_CHECK(h.version == 1 && h.kind == 2 && h.codec == 3);
	O3DS_CHECK(h.timestamp_us == 1000000ull && h.payload_size == 2 && h.seq == 0 && h.header_size == 20);

	unsigned char written[20] = {};
	Wire::WriteEnvelopeHeaderV1(written, Wire::EnvelopeKind::Control, Wire::EnvelopeCodec::O3DControl, 1000000ull, 2);
	O3DS_CHECK(std::memcmp(written, v1, 20) == 0);

	const unsigned char other[24] = { 'O', '3', 'D', 'X' };
	O3DS_CHECK(!Wire::HasEnvelopeMagic(other, sizeof(other)));
	O3DS_CHECK(!Wire::ReadEnvelopeHeader(other, sizeof(other), h));
	O3DS_CHECK(!Wire::ReadEnvelopeHeader(nullptr, 24, h));
	// A frame word is never an envelope.
	const unsigned char frame[24] = { 1, 0, 0, 0 };
	O3DS_CHECK(!Wire::HasEnvelopeMagic(frame, sizeof(frame)));
}

O3DS_TEST(Envelope_TimestampIsClampedNotUndefined)
{
	O3DS_CHECK(Wire::EnvelopeTimestampUs(1.5) == 1500000ull);
	O3DS_CHECK(Wire::EnvelopeTimestampUs(0.0) == 0);
	O3DS_CHECK(Wire::EnvelopeTimestampUs(-3.0) == 0);
	O3DS_CHECK(Wire::EnvelopeTimestampUs(std::numeric_limits<double>::quiet_NaN()) == 0);
	O3DS_CHECK(Wire::EnvelopeTimestampUs(std::numeric_limits<double>::infinity()) == 0);
	O3DS_CHECK(Wire::EnvelopeTimestampUs(1.0e300) == UINT64_MAX);
}

O3DS_TEST(NameHash_LengthPrefixedAndCaseSensitive)
{
	// The same bytes split differently are different lists.
	O3DS_CHECK(Wire::HashNames({ "ab", "c" }) != Wire::HashNames({ "a", "bc" }));
	O3DS_CHECK(Wire::HashNames({ "abc" }) != Wire::HashNames({ "abc", "" }));
	O3DS_CHECK(Wire::HashNames({}) != Wire::HashNames({ "" }));
	// Case is a real change.
	O3DS_CHECK(Wire::HashNames({ "Spine" }) != Wire::HashNames({ "spine" }));
	O3DS_CHECK(Wire::HashNames({ "Root", "Spine" }) == Wire::HashNames({ "Root", "Spine" }));

	// The incremental form is the same hash.
	uint64_t h = Wire::HashNamesBegin(2);
	h = Wire::HashNamesAdd(h, "Root", 4);
	h = Wire::HashNamesAdd(h, "Spine", 5);
	O3DS_CHECK(h == Wire::HashNames({ "Root", "Spine" }));

	// Known value: FNV-1a 64 over 00 00 00 00 (an empty list).
	uint64_t expected = Wire::kFnv64OffsetBasis;
	for (int i = 0; i < 4; ++i)
	{
		expected ^= 0;
		expected *= 1099511628211ull;
	}
	O3DS_CHECK(Wire::HashNamesBegin(0) == expected);

	// Parents follow the names and are length-prefixed too.
	const int32_t parentsA[2] = { -1, 0 };
	const int32_t parentsB[2] = { -1, 1 };
	const uint64_t names = Wire::HashNames({ "Root", "Spine" });
	O3DS_CHECK(Wire::HashParents(names, parentsA, 2) != Wire::HashParents(names, parentsB, 2));
	O3DS_CHECK(Wire::HashParents(names, parentsA, 1) != Wire::HashParents(names, parentsA, 2));
}

