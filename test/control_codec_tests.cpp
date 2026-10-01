// Tests for the control channel codec (src/o3ds/control.h: Value, Message,
// SerializeMessage, ParseMessage, Validate) and its limits
// (src/o3ds/parse_limits.h, ControlLimits). docs/adr/0011-control-channel.md,
// Verification: "Codec".
#include "test_framework.h"

#include "o3ds/control.h"
#include "o3ds/model.h"
#include "o3ds_control_generated.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace O3DS;
using namespace O3DS::Control;

namespace
{
	Message BaseMessage()
	{
		Message m;
		m.source_id = "6f1c2a3b4d5e6f708192a3b4c5d6e7f8";
		m.source_name = "BP_Stage_Sender";
		m.epoch = 1000;
		m.seq = 42;
		m.sender_time_us = 123456789;
		m.tx_wallclock_us = 1700000000000000ull;
		m.mocap_subjects = { "Hero", "Sidekick" };
		return m;
	}

	Entry MakeEntry(const std::string& key, const Value& value, uint64_t version, const std::string& target = std::string())
	{
		Entry e;
		e.key = key;
		e.target = target;
		e.value = value;
		e.version = version;
		return e;
	}

	std::vector<Value> EveryValueType()
	{
		TransformValue t;
		t.translation = { 1.5, -2.25, 1.0e9 };
		t.rotation = { 0.0, 0.70710678118654752, 0.0, 0.70710678118654752 };
		t.scale = { 1.0, 2.0, 0.5 };
		return {
			Value::MakeNone(),
			Value::MakeBool(true),
			Value::MakeInt(std::numeric_limits<int64_t>::min()),
			Value::MakeDouble(0.1 + 0.2),
			Value::MakeString("fog \xE2\x80\x94 thick"),
			Value::MakeName("emotion.joy"),
			Value::MakeVector3({ -1.0e-300, 3.0, 1.0e300 }),
			Value::MakeQuat({ 0.5, 0.5, 0.5, 0.5 }),
			Value::MakeTransform(t),
			Value::MakeColor({ 0.25f, 0.5f, 1.0f, 0.75f }),
			Value::MakeBytes({ 0, 1, 2, 254, 255 }),
		};
	}

	// Encodes `m` straight through the generated builder, skipping
	// Validate(), so tests can produce buffers the writer would refuse and
	// prove the reader rejects them.
	std::vector<uint8_t> EncodeUnchecked(const Message& m, double doubleValue)
	{
		namespace CD = O3DS::ControlData;
		flatbuffers::FlatBufferBuilder fbb;
		std::vector<flatbuffers::Offset<CD::Entry>> entries;
		for (const Entry& e : m.set)
		{
			const auto key = fbb.CreateString(e.key);
			const auto value = CD::CreateDoubleV(fbb, doubleValue).Union();
			entries.push_back(CD::CreateEntry(fbb, key, 0, CD::Value_DoubleV, value, e.version));
		}
		const auto sourceId = fbb.CreateString(m.source_id);
		flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<CD::Entry>>> setVec;
		if (!entries.empty())
			setVec = fbb.CreateVector(entries);
		const auto root = CD::CreateControlMessage(fbb, m.protocol_version, sourceId, 0, m.epoch, m.seq, m.sender_time_us,
			m.tx_wallclock_us, 0, setVec, 0, 0, m.snapshot_id, m.snapshot_part, m.snapshot_parts, m.snapshot_seq);
		CD::FinishControlMessageBuffer(fbb, root);
		return std::vector<uint8_t>(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
	}
}

O3DS_TEST(Control_RoundTrip_EveryValueTypeBitExact)
{
	// Four values per message, so every chunk stays well inside the budget.
	const std::vector<Value> values = EveryValueType();
	for (size_t first = 0; first < values.size(); first += 4)
	{
		Message m = BaseMessage();
		for (size_t k = first; k < values.size() && k < first + 4; ++k)
			m.set.push_back(MakeEntry("key." + std::to_string(k), values[k], 40 + (k % 3), k % 2 ? "Hero" : ""));
		O3DS_CHECK(Validate(m) == ParseError::None);

		std::vector<uint8_t> bytes;
		O3DS_CHECK(SerializeMessage(m, bytes));
		O3DS_CHECK(bytes.size() <= ControlLimits::kMaxPayloadBytes);

		Message back;
		O3DS_CHECK(ParseMessage(bytes.data(), bytes.size(), back) == ParseError::None);
		O3DS_CHECK_EQ(back.source_id, m.source_id);
		O3DS_CHECK_EQ(back.source_name, m.source_name);
		O3DS_CHECK_EQ(back.epoch, m.epoch);
		O3DS_CHECK_EQ(back.seq, m.seq);
		O3DS_CHECK_EQ(back.sender_time_us, m.sender_time_us);
		O3DS_CHECK_EQ(back.tx_wallclock_us, m.tx_wallclock_us);
		O3DS_CHECK(back.mocap_subjects == m.mocap_subjects);
		O3DS_CHECK_EQ(back.set.size(), m.set.size());
		for (size_t k = 0; k < m.set.size(); ++k)
		{
			O3DS_CHECK_EQ(back.set[k].key, m.set[k].key);
			O3DS_CHECK_EQ(back.set[k].target, m.set[k].target);
			O3DS_CHECK_EQ(back.set[k].version, m.set[k].version);
			O3DS_CHECK(back.set[k].value == m.set[k].value);
		}
	}
}

O3DS_TEST(Control_RoundTrip_EventsClearsAndSnapshotFields)
{
	Message live = BaseMessage();
	Clear c;
	c.key = "env.fog_density";
	c.version = 42;
	live.clear.push_back(c);
	Event ev;
	ev.event_id = 7;
	ev.name = "vfx.muzzle_flash";
	ev.target = "Hero";
	ev.value = Value::MakeName("left_hand");
	ev.ttl_ms = 500;
	ev.time_us = 123000000;
	live.events.push_back(ev);

	std::vector<uint8_t> bytes;
	O3DS_CHECK(SerializeMessage(live, bytes));
	Message back;
	O3DS_CHECK(ParseMessage(bytes.data(), bytes.size(), back) == ParseError::None);
	O3DS_CHECK_EQ(back.clear.size(), (size_t)1);
	O3DS_CHECK_EQ(back.clear[0].key, c.key);
	O3DS_CHECK_EQ(back.clear[0].version, c.version);
	O3DS_CHECK_EQ(back.events.size(), (size_t)1);
	O3DS_CHECK_EQ(back.events[0].event_id, ev.event_id);
	O3DS_CHECK_EQ(back.events[0].name, ev.name);
	O3DS_CHECK_EQ(back.events[0].target, ev.target);
	O3DS_CHECK_EQ(back.events[0].ttl_ms, ev.ttl_ms);
	O3DS_CHECK_EQ(back.events[0].time_us, ev.time_us);
	O3DS_CHECK(back.events[0].value == ev.value);
	O3DS_CHECK(!back.IsSnapshot());

	Message snap = BaseMessage();
	snap.snapshot_id = 9;
	snap.snapshot_part = 2;
	snap.snapshot_parts = 3;
	snap.snapshot_seq = 40;
	snap.set.push_back(MakeEntry("env.time_of_day", Value::MakeDouble(18.5), 40));
	O3DS_CHECK(SerializeMessage(snap, bytes));
	O3DS_CHECK(ParseMessage(bytes.data(), bytes.size(), back) == ParseError::None);
	O3DS_CHECK(back.IsSnapshot());
	O3DS_CHECK_EQ(back.snapshot_id, (uint32_t)9);
	O3DS_CHECK_EQ(back.snapshot_part, (uint16_t)2);
	O3DS_CHECK_EQ(back.snapshot_parts, (uint16_t)3);
	O3DS_CHECK_EQ(back.snapshot_seq, (uint64_t)40);
}

O3DS_TEST(Control_SizeBudget_WriterRefusesOneByteOver)
{
	// Grow a Bytes value one byte at a time across two entries until the
	// writer refuses: the last accepted size must be within the budget and
	// close to it, and the refusal must be for size alone (Validate passes).
	Message m = BaseMessage();
	m.set.push_back(MakeEntry("a", Value::MakeBytes(std::vector<uint8_t>(ControlLimits::kMaxBytesValueBytes, 0xAB)), 42));
	m.set.push_back(MakeEntry("b", Value::MakeBytes({}), 42));

	size_t lastOk = 0;
	bool refused = false;
	for (size_t n = 0; n <= ControlLimits::kMaxBytesValueBytes; ++n)
	{
		m.set[1].value.bytes.assign(n, 0xCD);
		std::vector<uint8_t> bytes;
		if (SerializeMessage(m, bytes))
		{
			O3DS_CHECK(bytes.size() <= ControlLimits::kMaxPayloadBytes);
			lastOk = bytes.size();
			continue;
		}
		O3DS_CHECK(Validate(m) == ParseError::None);
		refused = true;
		break;
	}
	O3DS_CHECK(refused);
	O3DS_CHECK(lastOk + 16 > ControlLimits::kMaxPayloadBytes);
}

O3DS_TEST(Control_Reader_RejectsOverBudgetBuffer)
{
	std::vector<uint8_t> big(ControlLimits::kMaxPayloadBytes + 1, 0);
	Message out;
	O3DS_CHECK(ParseMessage(big.data(), big.size(), out) == ParseError::TooLarge);
	O3DS_CHECK(ParseMessage(nullptr, 10, out) == ParseError::Empty);
	O3DS_CHECK(ParseMessage(big.data(), 0, out) == ParseError::Empty);
}

O3DS_TEST(Control_Validate_EachLimitHasItsOwnError)
{
	{
		Message m = BaseMessage();
		m.protocol_version = kProtocolVersion + 1;
		O3DS_CHECK(Validate(m) == ParseError::UnsupportedVersion);
	}
	{
		Message m = BaseMessage();
		m.source_id.clear();
		O3DS_CHECK(Validate(m) == ParseError::MissingField);
	}
	{
		Message m = BaseMessage();
		m.seq = 0;
		O3DS_CHECK(Validate(m) == ParseError::MissingField);
	}
	{
		Message m = BaseMessage();
		m.set.push_back(MakeEntry(std::string(ControlLimits::kMaxKeyBytes + 1, 'k'), Value::MakeBool(true), 42));
		O3DS_CHECK(Validate(m) == ParseError::StringTooLong);
	}
	{
		Message m = BaseMessage();
		m.set.push_back(MakeEntry("bad\xC3", Value::MakeBool(true), 42));
		O3DS_CHECK(Validate(m) == ParseError::BadUtf8);
	}
	{
		Message m = BaseMessage();
		m.set.push_back(MakeEntry("light.intensity", Value::MakeDouble(std::numeric_limits<double>::quiet_NaN()), 42));
		O3DS_CHECK(Validate(m) == ParseError::NonFinite);
	}
	{
		Message m = BaseMessage();
		m.set.push_back(MakeEntry("light.color", Value::MakeColor({ 1.0f, std::numeric_limits<float>::infinity(), 0.0f, 1.0f }), 42));
		O3DS_CHECK(Validate(m) == ParseError::NonFinite);
	}
	{
		Message m = BaseMessage();
		for (size_t k = 0; k <= ControlLimits::kMaxItemsPerMessage; ++k)
			m.set.push_back(MakeEntry("k" + std::to_string(k), Value::MakeBool(true), 42));
		O3DS_CHECK(Validate(m) == ParseError::TooManyItems);
	}
	{
		Message m = BaseMessage();
		m.set.push_back(MakeEntry("k", Value::MakeBool(true), 0));
		O3DS_CHECK(Validate(m) == ParseError::BadVersion);
		m.set[0].version = m.seq + 1;
		O3DS_CHECK(Validate(m) == ParseError::BadVersion);
	}
	{
		Message m = BaseMessage();
		Event ev;
		ev.event_id = 1;
		ev.name = "cue";
		ev.ttl_ms = 0;
		m.events.push_back(ev);
		O3DS_CHECK(Validate(m) == ParseError::BadTtl);
		m.events[0].ttl_ms = ControlLimits::kMaxEventTtlMs + 1;
		O3DS_CHECK(Validate(m) == ParseError::BadTtl);
	}
	{
		Message m = BaseMessage();
		m.snapshot_id = 1;
		m.snapshot_parts = 2;
		m.snapshot_part = 2; // out of range
		m.snapshot_seq = 40;
		O3DS_CHECK(Validate(m) == ParseError::BadSnapshot);
	}
	{
		Message m = BaseMessage();
		m.snapshot_id = 1;
		m.snapshot_parts = 1;
		m.snapshot_seq = 40;
		Event ev;
		ev.event_id = 1;
		ev.name = "cue";
		m.events.push_back(ev); // events never ride in a snapshot
		O3DS_CHECK(Validate(m) == ParseError::BadSnapshot);
	}
	{
		Message m = BaseMessage();
		m.snapshot_seq = 5; // snapshot field on a live message
		O3DS_CHECK(Validate(m) == ParseError::BadSnapshot);
	}
	{
		Message m = BaseMessage();
		m.mocap_subjects.assign(ControlLimits::kMaxMocapSubjects + 1, "S");
		O3DS_CHECK(Validate(m) == ParseError::StringTooLong);
	}
	{
		Message m = BaseMessage();
		m.set.push_back(MakeEntry("k", Value::MakeString(std::string(ControlLimits::kMaxStringValueBytes + 1, 's')), 42));
		O3DS_CHECK(Validate(m) == ParseError::StringTooLong);
	}
	{
		Message m = BaseMessage();
		m.set.push_back(MakeEntry("k", Value::MakeBytes(std::vector<uint8_t>(ControlLimits::kMaxBytesValueBytes + 1)), 42));
		O3DS_CHECK(Validate(m) == ParseError::StringTooLong);
	}
}

O3DS_TEST(Control_Reader_RejectsWhatTheWriterWouldRefuse)
{
	// Hand-built buffers the writer would never produce: the reader must
	// apply the same Validate() rules.
	Message m = BaseMessage();
	m.set.push_back(MakeEntry("light.intensity", Value::MakeDouble(0.0), 42));
	Message out;

	std::vector<uint8_t> nan = EncodeUnchecked(m, std::numeric_limits<double>::quiet_NaN());
	O3DS_CHECK(ParseMessage(nan.data(), nan.size(), out) == ParseError::NonFinite);
	O3DS_CHECK(out.set.empty());

	m.protocol_version = 2;
	std::vector<uint8_t> v2 = EncodeUnchecked(m, 1.0);
	O3DS_CHECK(ParseMessage(v2.data(), v2.size(), out) == ParseError::UnsupportedVersion);
}

O3DS_TEST(Control_Reader_RejectsTruncationAndGarbage)
{
	Message m = BaseMessage();
	m.set.push_back(MakeEntry("env.fog_density", Value::MakeDouble(0.3), 42));
	std::vector<uint8_t> bytes;
	O3DS_CHECK(SerializeMessage(m, bytes));

	// FlatBuffers pads the end of a buffer, so dropping trailing padding
	// can leave a complete message. Whatever a prefix parses to, it must be
	// exactly the original: truncation never yields a different message.
	Message out;
	size_t rejected = 0;
	for (size_t n = 1; n < bytes.size(); ++n)
	{
		if (ParseMessage(bytes.data(), n, out) != ParseError::None)
		{
			++rejected;
			continue;
		}
		O3DS_CHECK(n + 8 >= bytes.size()); // only padding can be missing
		O3DS_CHECK_EQ(out.set.size(), (size_t)1);
		O3DS_CHECK(out.set[0].value == m.set[0].value);
	}
	O3DS_CHECK(rejected + 8 >= bytes.size() - 1);

	std::vector<uint8_t> garbage(200);
	for (size_t k = 0; k < garbage.size(); ++k)
		garbage[k] = static_cast<uint8_t>(k * 37 + 11);
	O3DS_CHECK(ParseMessage(garbage.data(), garbage.size(), out) != ParseError::None);

	// The right identifier on a corrupted body still fails verification.
	std::vector<uint8_t> corrupt = bytes;
	for (size_t k = 8; k < corrupt.size(); k += 3)
		corrupt[k] ^= 0x5A;
	O3DS_CHECK(ParseMessage(corrupt.data(), corrupt.size(), out) != ParseError::None);
}

O3DS_TEST(Control_ParsesAtAnyAlignment)
{
	Message m = BaseMessage();
	m.set.push_back(MakeEntry("pos", Value::MakeVector3({ 1.0, 2.0, 3.0 }), 42));
	std::vector<uint8_t> bytes;
	O3DS_CHECK(SerializeMessage(m, bytes));
	for (size_t offset = 0; offset < 8; ++offset)
	{
		std::vector<uint8_t> shifted(offset, 0);
		shifted.insert(shifted.end(), bytes.begin(), bytes.end());
		Message out;
		O3DS_CHECK(ParseMessage(shifted.data() + offset, bytes.size(), out) == ParseError::None);
		O3DS_CHECK(out.set[0].value == m.set[0].value);
	}
}

O3DS_TEST(Control_And_SubjectList_NeverParseAsEachOther)
{
	// A control payload fed to the mocap parser...
	Message m = BaseMessage();
	m.set.push_back(MakeEntry("env.fog_density", Value::MakeDouble(0.3), 42));
	std::vector<uint8_t> control;
	O3DS_CHECK(SerializeMessage(m, control));
	SubjectList list;
	O3DS_CHECK(!list.Parse(reinterpret_cast<const char*>(control.data()), control.size()));

	// ...and a mocap frame (header and FlatBuffer) fed to the control parser.
	SubjectList mocap;
	Subject* subject = mocap.addSubject("Hero");
	subject->addTransform("Root", -1);
	std::vector<char> frame;
	O3DS_CHECK(mocap.Serialize(frame, 1.0) > 0);
	Message out;
	O3DS_CHECK(ParseMessage(reinterpret_cast<const uint8_t*>(frame.data()), frame.size(), out) != ParseError::None);
	O3DS_CHECK(ParseMessage(reinterpret_cast<const uint8_t*>(frame.data()) + 8, frame.size() - 8, out) == ParseError::BadIdentifier);
}

O3DS_TEST(Control_Utf8Validator)
{
	O3DS_CHECK(IsValidUtf8(""));
	O3DS_CHECK(IsValidUtf8("ascii.key_01"));
	O3DS_CHECK(IsValidUtf8("caf\xC3\xA9"));                // 2-byte
	O3DS_CHECK(IsValidUtf8("\xE2\x82\xAC"));               // 3-byte (euro)
	O3DS_CHECK(IsValidUtf8("\xF0\x9F\x8E\xAC"));           // 4-byte (clapper)
	O3DS_CHECK(!IsValidUtf8(std::string("a\0b", 3)));      // embedded NUL
	O3DS_CHECK(!IsValidUtf8("\xC3"));                      // truncated
	O3DS_CHECK(!IsValidUtf8("\xE2\x82"));                  // truncated
	O3DS_CHECK(!IsValidUtf8("\x80"));                      // stray continuation
	O3DS_CHECK(!IsValidUtf8("\xC0\xAF"));                  // overlong
	O3DS_CHECK(!IsValidUtf8("\xED\xA0\x80"));              // surrogate
	O3DS_CHECK(!IsValidUtf8("\xF4\x90\x80\x80"));          // above U+10FFFF
	O3DS_CHECK(!IsValidUtf8("\xFF"));
}

O3DS_TEST(Control_UnknownValueTypeIsSkippedNotRejected)
{
	// A newer writer appends a union member this reader does not know. The
	// message still parses; that item is marked unsupported, the receiver
	// skips it and counts it, and a snapshot still counts its key as present.
	namespace CD = O3DS::ControlData;
	flatbuffers::FlatBufferBuilder fbb;
	const auto keyNew = fbb.CreateString("a.new_type");
	const auto futureValue = CD::CreateBoolV(fbb, true).Union(); // any table will do
	const auto keyOld = fbb.CreateString("b.double");
	const auto oldValue = CD::CreateDoubleV(fbb, 2.5).Union();
	std::vector<flatbuffers::Offset<CD::Entry>> entries = {
		CD::CreateEntry(fbb, keyNew, 0, static_cast<CD::Value>(CD::Value_MAX + 1), futureValue, 40),
		CD::CreateEntry(fbb, keyOld, 0, CD::Value_DoubleV, oldValue, 40),
	};
	const auto setVec = fbb.CreateVector(entries);
	const auto sourceId = fbb.CreateString("6f1c2a3b4d5e6f708192a3b4c5d6e7f8");
	const auto root = CD::CreateControlMessage(fbb, 1, sourceId, 0, 1000, 42, 1, 0, 0, setVec, 0, 0, 7, 0, 1, 41);
	CD::FinishControlMessageBuffer(fbb, root);

	Message parsed;
	O3DS_CHECK(ParseMessage(fbb.GetBufferPointer(), fbb.GetSize(), parsed) == ParseError::None);
	O3DS_CHECK_EQ(parsed.set.size(), (size_t)2);
	O3DS_CHECK(!parsed.set[0].supported);
	O3DS_CHECK(parsed.set[1].supported);

	std::vector<uint8_t> again;
	O3DS_CHECK(!SerializeMessage(parsed, again)); // a reader never re-sends what it cannot encode

	ControlReceiver rx;
	std::vector<Change> out;
	Message live = BaseMessage();
	live.set.push_back(MakeEntry("a.new_type", Value::MakeInt(1), 30));
	rx.Apply(live, 100, 0.0, out);
	rx.Apply(parsed, fbb.GetSize(), 0.1, out); // complete one-part snapshot
	O3DS_CHECK_EQ(rx.GetStats().items_unsupported, (uint64_t)1);
	O3DS_CHECK(rx.FindValue(parsed.source_id, "a.new_type", "") != nullptr); // present at capture: kept
	O3DS_CHECK(rx.FindValue(parsed.source_id, "b.double", "") != nullptr);
}

O3DS_TEST(Control_SnapshotPartsMustBeSortedAndOnlyEmptyWhenSinglePart)
{
	Message m = BaseMessage();
	m.snapshot_id = 1;
	m.snapshot_parts = 2;
	m.snapshot_seq = 40;
	O3DS_CHECK(Validate(m) == ParseError::BadSnapshot); // empty part of a multi-part snapshot
	m.snapshot_parts = 1;
	O3DS_CHECK(Validate(m) == ParseError::None);        // snapshot of an empty table

	m.set.push_back(MakeEntry("b", Value::MakeBool(true), 40));
	m.set.push_back(MakeEntry("a", Value::MakeBool(true), 40));
	O3DS_CHECK(Validate(m) == ParseError::BadSnapshot); // out of order
	std::swap(m.set[0], m.set[1]);
	O3DS_CHECK(Validate(m) == ParseError::None);
	m.set.push_back(MakeEntry("b", Value::MakeBool(true), 40));
	O3DS_CHECK(Validate(m) == ParseError::BadSnapshot); // duplicate
	m.set.back().target = "Hero";                       // ("b", "Hero") follows ("b", "")
	O3DS_CHECK(Validate(m) == ParseError::None);
}
