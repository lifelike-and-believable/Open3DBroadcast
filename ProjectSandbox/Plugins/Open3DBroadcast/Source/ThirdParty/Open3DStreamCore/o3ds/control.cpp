/*
Open 3D Stream

Copyright 2026 Alastair Macleod

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "control.h"
#include "o3ds_control_generated.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace O3DS
{
	namespace Control
	{
		namespace CL = ControlLimits;
		namespace CD = ControlData;

		// ------------------------------------------------------------------
		// Value
		// ------------------------------------------------------------------

		Value Value::MakeBool(bool v) { Value r; r.type = ValueType::Bool; r.b = v; return r; }
		Value Value::MakeInt(int64_t v) { Value r; r.type = ValueType::Int; r.i = v; return r; }
		Value Value::MakeDouble(double v) { Value r; r.type = ValueType::Double; r.d = v; return r; }
		Value Value::MakeString(std::string v) { Value r; r.type = ValueType::String; r.text = std::move(v); return r; }
		Value Value::MakeName(std::string v) { Value r; r.type = ValueType::Name; r.text = std::move(v); return r; }
		Value Value::MakeVector3(const Vec3& v) { Value r; r.type = ValueType::Vector3; r.vec = v; return r; }
		Value Value::MakeQuat(const Quat& v) { Value r; r.type = ValueType::Quat; r.quat = v; return r; }
		Value Value::MakeTransform(const TransformValue& v) { Value r; r.type = ValueType::Transform; r.transform = v; return r; }
		Value Value::MakeColor(const Color& v) { Value r; r.type = ValueType::Color; r.color = v; return r; }
		Value Value::MakeBytes(std::vector<uint8_t> v) { Value r; r.type = ValueType::Bytes; r.bytes = std::move(v); return r; }

		namespace
		{
			bool Eq(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
			bool Eq(const Quat& a, const Quat& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
		}

		bool Value::operator==(const Value& o) const
		{
			if (type != o.type)
				return false;
			switch (type)
			{
			case ValueType::None: return true;
			case ValueType::Bool: return b == o.b;
			case ValueType::Int: return i == o.i;
			case ValueType::Double: return d == o.d;
			case ValueType::String:
			case ValueType::Name: return text == o.text;
			case ValueType::Vector3: return Eq(vec, o.vec);
			case ValueType::Quat: return Eq(quat, o.quat);
			case ValueType::Transform:
				return Eq(transform.translation, o.transform.translation) && Eq(transform.rotation, o.transform.rotation)
					&& Eq(transform.scale, o.transform.scale);
			case ValueType::Color:
				return color.r == o.color.r && color.g == o.color.g && color.b == o.color.b && color.a == o.color.a;
			case ValueType::Bytes: return bytes == o.bytes;
			}
			return false;
		}

		// ------------------------------------------------------------------
		// Validation
		// ------------------------------------------------------------------

		const char* ToString(ParseError error)
		{
			switch (error)
			{
			case ParseError::None: return "ok";
			case ParseError::Empty: return "empty buffer";
			case ParseError::TooLarge: return "message larger than the control size budget";
			case ParseError::BadIdentifier: return "not a control message (identifier is not O3DC)";
			case ParseError::VerifyFailed: return "FlatBuffers verification failed";
			case ParseError::UnsupportedVersion: return "unsupported control protocol version";
			case ParseError::MissingField: return "a required field is missing or zero";
			case ParseError::TooManyItems: return "too many items in one message";
			case ParseError::StringTooLong: return "a string or list is longer than allowed";
			case ParseError::BadUtf8: return "a string is not valid UTF-8";
			case ParseError::NonFinite: return "a numeric value is NaN or infinite";
			case ParseError::BadValue: return "malformed or unknown value";
			case ParseError::BadSnapshot: return "inconsistent snapshot fields";
			case ParseError::BadVersion: return "entry version is 0 or above the message seq";
			case ParseError::BadTtl: return "event TTL out of range";
			}
			return "unknown";
		}

		bool IsValidUtf8(const std::string& text)
		{
			const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
			const size_t n = text.size();
			size_t i = 0;
			while (i < n)
			{
				const unsigned char c = p[i];
				if (c == 0)
					return false;
				if (c < 0x80)
				{
					++i;
					continue;
				}
				size_t extra = 0;
				uint32_t cp = 0;
				if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
				else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
				else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
				else return false;
				if (i + extra >= n)
					return false; // truncated sequence
				for (size_t k = 1; k <= extra; ++k)
				{
					const unsigned char cc = p[i + k];
					if ((cc & 0xC0) != 0x80)
						return false;
					cp = (cp << 6) | (cc & 0x3F);
				}
				// Overlong forms, surrogates and out-of-range code points.
				if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000)
					|| (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
					return false;
				i += extra + 1;
			}
			return true;
		}

		namespace
		{
			ParseError CheckText(const std::string& text, size_t maxBytes, bool required)
			{
				if (required && text.empty())
					return ParseError::MissingField;
				if (text.size() > maxBytes)
					return ParseError::StringTooLong;
				if (!IsValidUtf8(text))
					return ParseError::BadUtf8;
				return ParseError::None;
			}

			bool Finite(double v) { return std::isfinite(v); }
			bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
			bool Finite(const Quat& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z) && Finite(v.w); }

			ParseError CheckValue(const Value& v)
			{
				switch (v.type)
				{
				case ValueType::None:
				case ValueType::Bool:
				case ValueType::Int:
					return ParseError::None;
				case ValueType::Double:
					return Finite(v.d) ? ParseError::None : ParseError::NonFinite;
				case ValueType::String:
					return CheckText(v.text, CL::kMaxStringValueBytes, false);
				case ValueType::Name:
					return CheckText(v.text, CL::kMaxKeyBytes, false);
				case ValueType::Vector3:
					return Finite(v.vec) ? ParseError::None : ParseError::NonFinite;
				case ValueType::Quat:
					return Finite(v.quat) ? ParseError::None : ParseError::NonFinite;
				case ValueType::Transform:
					return (Finite(v.transform.translation) && Finite(v.transform.rotation) && Finite(v.transform.scale))
						? ParseError::None : ParseError::NonFinite;
				case ValueType::Color:
					return (std::isfinite(v.color.r) && std::isfinite(v.color.g) && std::isfinite(v.color.b) && std::isfinite(v.color.a))
						? ParseError::None : ParseError::NonFinite;
				case ValueType::Bytes:
					return v.bytes.size() <= CL::kMaxBytesValueBytes ? ParseError::None : ParseError::StringTooLong;
				}
				return ParseError::BadValue;
			}

#define O3DS_CONTROL_TRY(expr) do { const ParseError e_ = (expr); if (e_ != ParseError::None) return e_; } while (0)
		}

		ParseError Validate(const Message& m)
		{
			if (m.protocol_version == 0 || m.protocol_version > kProtocolVersion)
				return ParseError::UnsupportedVersion;
			O3DS_CONTROL_TRY(CheckText(m.source_id, CL::kMaxSourceIdBytes, true));
			O3DS_CONTROL_TRY(CheckText(m.source_name, CL::kMaxSourceNameBytes, false));
			if (m.epoch == 0 || m.seq == 0)
				return ParseError::MissingField;

			if (m.mocap_subjects.size() > CL::kMaxMocapSubjects)
				return ParseError::StringTooLong;
			size_t subjectBytes = 0;
			for (const std::string& s : m.mocap_subjects)
			{
				O3DS_CONTROL_TRY(CheckText(s, CL::kMaxMocapSubjectBytes, true));
				subjectBytes += s.size();
			}
			if (subjectBytes > CL::kMaxMocapSubjectsTotalBytes)
				return ParseError::StringTooLong;

			if (m.ItemCount() > CL::kMaxItemsPerMessage)
				return ParseError::TooManyItems;

			const uint64_t versionCeiling = m.IsSnapshot() ? m.snapshot_seq : m.seq;
			for (const Entry& e : m.set)
			{
				O3DS_CONTROL_TRY(CheckText(e.key, CL::kMaxKeyBytes, true));
				O3DS_CONTROL_TRY(CheckText(e.target, CL::kMaxTargetBytes, false));
				if (e.supported)
					O3DS_CONTROL_TRY(CheckValue(e.value));
				if (e.version == 0 || e.version > versionCeiling)
					return ParseError::BadVersion;
			}
			for (const Clear& c : m.clear)
			{
				O3DS_CONTROL_TRY(CheckText(c.key, CL::kMaxKeyBytes, true));
				O3DS_CONTROL_TRY(CheckText(c.target, CL::kMaxTargetBytes, false));
				if (c.version == 0 || c.version > m.seq)
					return ParseError::BadVersion;
			}
			for (const Event& ev : m.events)
			{
				if (ev.event_id == 0)
					return ParseError::MissingField;
				O3DS_CONTROL_TRY(CheckText(ev.name, CL::kMaxKeyBytes, true));
				O3DS_CONTROL_TRY(CheckText(ev.target, CL::kMaxTargetBytes, false));
				if (ev.supported)
					O3DS_CONTROL_TRY(CheckValue(ev.value));
				if (ev.ttl_ms == 0 || ev.ttl_ms > CL::kMaxEventTtlMs)
					return ParseError::BadTtl;
			}

			if (m.IsSnapshot())
			{
				// A snapshot part carries only entries: clears and events are live.
				if (m.snapshot_parts == 0 || m.snapshot_parts > CL::kMaxSnapshotParts || m.snapshot_part >= m.snapshot_parts
					|| m.snapshot_seq == 0 || m.snapshot_seq > m.seq || !m.clear.empty() || !m.events.empty())
					return ParseError::BadSnapshot;
				// Parts cover contiguous ranges of the sorted table: entries in
				// strictly increasing (key, target) order, and only a snapshot
				// of an empty table has an empty part.
				if (m.set.empty() && m.snapshot_parts != 1)
					return ParseError::BadSnapshot;
				for (size_t k = 1; k < m.set.size(); ++k)
				{
					if (!(SlotKey(m.set[k - 1].key, m.set[k - 1].target) < SlotKey(m.set[k].key, m.set[k].target)))
						return ParseError::BadSnapshot;
				}
			}
			else if (m.snapshot_part != 0 || m.snapshot_parts != 0 || m.snapshot_seq != 0)
			{
				return ParseError::BadSnapshot;
			}
			return ParseError::None;
		}

		// ------------------------------------------------------------------
		// Codec
		// ------------------------------------------------------------------

		namespace
		{
			CD::Vec3 ToWire(const Vec3& v) { return CD::Vec3(v.x, v.y, v.z); }
			CD::Quat ToWire(const Quat& v) { return CD::Quat(v.x, v.y, v.z, v.w); }

			Vec3 FromWire(const CD::Vec3* v, const Vec3& fallback)
			{
				if (v == nullptr)
					return fallback;
				Vec3 r;
				r.x = v->x(); r.y = v->y(); r.z = v->z();
				return r;
			}

			Quat FromWire(const CD::Quat* v)
			{
				Quat r;
				if (v != nullptr)
				{
					r.x = v->x(); r.y = v->y(); r.z = v->z(); r.w = v->w();
				}
				return r;
			}

			flatbuffers::Offset<void> WriteValue(flatbuffers::FlatBufferBuilder& fbb, const Value& v, CD::Value& outType)
			{
				switch (v.type)
				{
				case ValueType::None:
					outType = CD::Value_NONE;
					return 0;
				case ValueType::Bool:
					outType = CD::Value_BoolV;
					return CD::CreateBoolV(fbb, v.b).Union();
				case ValueType::Int:
					outType = CD::Value_IntV;
					return CD::CreateIntV(fbb, v.i).Union();
				case ValueType::Double:
					outType = CD::Value_DoubleV;
					return CD::CreateDoubleV(fbb, v.d).Union();
				case ValueType::String:
				{
					outType = CD::Value_StringV;
					const auto s = fbb.CreateString(v.text);
					return CD::CreateStringV(fbb, s).Union();
				}
				case ValueType::Name:
				{
					outType = CD::Value_NameV;
					const auto s = fbb.CreateString(v.text);
					return CD::CreateNameV(fbb, s).Union();
				}
				case ValueType::Vector3:
				{
					outType = CD::Value_Vec3V;
					const CD::Vec3 w = ToWire(v.vec);
					return CD::CreateVec3V(fbb, &w).Union();
				}
				case ValueType::Quat:
				{
					outType = CD::Value_QuatV;
					const CD::Quat w = ToWire(v.quat);
					return CD::CreateQuatV(fbb, &w).Union();
				}
				case ValueType::Transform:
				{
					outType = CD::Value_TransformV;
					const CD::Vec3 t = ToWire(v.transform.translation);
					const CD::Quat r = ToWire(v.transform.rotation);
					const CD::Vec3 s = ToWire(v.transform.scale);
					return CD::CreateTransformV(fbb, &t, &r, &s).Union();
				}
				case ValueType::Color:
				{
					outType = CD::Value_ColorV;
					const CD::Color w(v.color.r, v.color.g, v.color.b, v.color.a);
					return CD::CreateColorV(fbb, &w).Union();
				}
				case ValueType::Bytes:
				{
					outType = CD::Value_BytesV;
					const auto b = fbb.CreateVector(v.bytes);
					return CD::CreateBytesV(fbb, b).Union();
				}
				}
				outType = CD::Value_NONE;
				return 0;
			}

			std::string ReadString(const flatbuffers::String* s)
			{
				return s == nullptr ? std::string() : std::string(s->c_str(), s->size());
			}

			// Reads a known union member from a table with value_as_*()
			// accessors (Entry or Event). Returns false for a type this reader
			// does not know, or a member whose table is missing.
			template <typename TableT>
			bool ReadKnownValue(const TableT& table, Value& out)
			{
				out = Value();
				switch (table.value_type())
				{
				case CD::Value_NONE:
					return true;
				case CD::Value_BoolV:
					if (const CD::BoolV* v = table.value_as_BoolV()) { out = Value::MakeBool(v->v()); return true; }
					return false;
				case CD::Value_IntV:
					if (const CD::IntV* v = table.value_as_IntV()) { out = Value::MakeInt(v->v()); return true; }
					return false;
				case CD::Value_DoubleV:
					if (const CD::DoubleV* v = table.value_as_DoubleV()) { out = Value::MakeDouble(v->v()); return true; }
					return false;
				case CD::Value_StringV:
					if (const CD::StringV* v = table.value_as_StringV()) { out = Value::MakeString(ReadString(v->v())); return true; }
					return false;
				case CD::Value_NameV:
					if (const CD::NameV* v = table.value_as_NameV()) { out = Value::MakeName(ReadString(v->v())); return true; }
					return false;
				case CD::Value_Vec3V:
					if (const CD::Vec3V* v = table.value_as_Vec3V()) { out = Value::MakeVector3(FromWire(v->v(), Vec3())); return true; }
					return false;
				case CD::Value_QuatV:
					if (const CD::QuatV* v = table.value_as_QuatV()) { out = Value::MakeQuat(FromWire(v->v())); return true; }
					return false;
				case CD::Value_TransformV:
					if (const CD::TransformV* v = table.value_as_TransformV())
					{
						TransformValue t;
						t.translation = FromWire(v->t(), Vec3());
						t.rotation = FromWire(v->r());
						t.scale = FromWire(v->s(), TransformValue().scale);
						out = Value::MakeTransform(t);
						return true;
					}
					return false;
				case CD::Value_ColorV:
					if (const CD::ColorV* v = table.value_as_ColorV())
					{
						Color c;
						if (const CD::Color* w = v->v())
						{
							c.r = w->r(); c.g = w->g(); c.b = w->b(); c.a = w->a();
						}
						out = Value::MakeColor(c);
						return true;
					}
					return false;
				case CD::Value_BytesV:
					if (const CD::BytesV* v = table.value_as_BytesV())
					{
						std::vector<uint8_t> bytes;
						if (const flatbuffers::Vector<uint8_t>* b = v->v())
							bytes.assign(b->begin(), b->end());
						out = Value::MakeBytes(std::move(bytes));
						return true;
					}
					return false;
				}
				return false;
			}

			enum class ReadResult { Ok, Unknown, Bad };

			// Unknown: a union member newer than this reader (the generated
			// verifier accepts those, `default: return true`). Bad: a known
			// member whose table is missing.
			template <typename TableT>
			ReadResult ReadValue(const TableT& table, Value& out)
			{
				if (ReadKnownValue(table, out))
					return ReadResult::Ok;
				return table.value_type() > CD::Value_MAX ? ReadResult::Unknown : ReadResult::Bad;
			}

			// Encodes without validating; callers validate first.
			void Encode(const Message& m, std::vector<uint8_t>& out)
			{
				flatbuffers::FlatBufferBuilder fbb(512);

				std::vector<flatbuffers::Offset<flatbuffers::String>> subjects;
				subjects.reserve(m.mocap_subjects.size());
				for (const std::string& s : m.mocap_subjects)
					subjects.push_back(fbb.CreateString(s));

				std::vector<flatbuffers::Offset<CD::Entry>> entries;
				entries.reserve(m.set.size());
				for (const Entry& e : m.set)
				{
					const auto key = fbb.CreateString(e.key);
					const auto target = e.target.empty() ? flatbuffers::Offset<flatbuffers::String>() : fbb.CreateString(e.target);
					CD::Value type = CD::Value_NONE;
					const auto value = WriteValue(fbb, e.value, type);
					entries.push_back(CD::CreateEntry(fbb, key, target, type, value, e.version));
				}

				std::vector<flatbuffers::Offset<CD::Clear>> clears;
				clears.reserve(m.clear.size());
				for (const Clear& c : m.clear)
				{
					const auto key = fbb.CreateString(c.key);
					const auto target = c.target.empty() ? flatbuffers::Offset<flatbuffers::String>() : fbb.CreateString(c.target);
					clears.push_back(CD::CreateClear(fbb, key, target, c.version));
				}

				std::vector<flatbuffers::Offset<CD::Event>> events;
				events.reserve(m.events.size());
				for (const Event& ev : m.events)
				{
					const auto name = fbb.CreateString(ev.name);
					const auto target = ev.target.empty() ? flatbuffers::Offset<flatbuffers::String>() : fbb.CreateString(ev.target);
					CD::Value type = CD::Value_NONE;
					const auto value = WriteValue(fbb, ev.value, type);
					events.push_back(CD::CreateEvent(fbb, ev.event_id, name, target, type, value, ev.ttl_ms, ev.time_us));
				}

				const auto sourceId = fbb.CreateString(m.source_id);
				const auto sourceName = m.source_name.empty() ? flatbuffers::Offset<flatbuffers::String>() : fbb.CreateString(m.source_name);
				flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> subjectsVec;
				flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<CD::Entry>>> setVec;
				flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<CD::Clear>>> clearVec;
				flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<CD::Event>>> eventsVec;
				if (!subjects.empty()) subjectsVec = fbb.CreateVector(subjects);
				if (!entries.empty()) setVec = fbb.CreateVector(entries);
				if (!clears.empty()) clearVec = fbb.CreateVector(clears);
				if (!events.empty()) eventsVec = fbb.CreateVector(events);

				const auto root = CD::CreateControlMessage(fbb, m.protocol_version, sourceId, sourceName, m.epoch, m.seq,
					m.sender_time_us, m.tx_wallclock_us, subjectsVec, setVec, clearVec, eventsVec,
					m.snapshot_id, m.snapshot_part, m.snapshot_parts, m.snapshot_seq);
				CD::FinishControlMessageBuffer(fbb, root);

				out.assign(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
			}
		}

		bool SerializeMessage(const Message& message, std::vector<uint8_t>& out)
		{
			out.clear();
			if (Validate(message) != ParseError::None)
				return false;
			for (const Entry& e : message.set)
			{
				if (!e.supported)
					return false;
			}
			for (const Event& ev : message.events)
			{
				if (!ev.supported)
					return false;
			}
			Encode(message, out);
			if (out.size() > CL::kMaxPayloadBytes)
			{
				out.clear();
				return false;
			}
			return true;
		}

		ParseError ParseMessage(const uint8_t* data, size_t len, Message& out)
		{
			out = Message();
			if (data == nullptr || len == 0)
				return ParseError::Empty;
			if (len > CL::kMaxPayloadBytes)
				return ParseError::TooLarge;

			// Copy into a heap buffer: network payloads sit at arbitrary
			// offsets inside envelopes, and FlatBuffers reads 8-byte scalars
			// in place.
			const std::vector<uint8_t> aligned(data, data + len);
			const uint8_t* buf = aligned.data();

			if (len < 8 || !CD::ControlMessageBufferHasIdentifier(buf))
				return ParseError::BadIdentifier;
			flatbuffers::Verifier verifier(buf, len);
			if (!CD::VerifyControlMessageBuffer(verifier))
				return ParseError::VerifyFailed;

			const CD::ControlMessage* root = CD::GetControlMessage(buf);
			Message m;
			m.protocol_version = root->protocol_version();
			m.source_id = ReadString(root->source_id());
			m.source_name = ReadString(root->source_name());
			m.epoch = root->epoch();
			m.seq = root->seq();
			m.sender_time_us = root->sender_time_us();
			m.tx_wallclock_us = root->tx_wallclock_us();
			m.snapshot_id = root->snapshot_id();
			m.snapshot_part = root->snapshot_part();
			m.snapshot_parts = root->snapshot_parts();
			m.snapshot_seq = root->snapshot_seq();

			// Check counts before copying, so a hostile buffer cannot make us
			// allocate per element beyond the limits.
			const auto* subjects = root->mocap_subjects();
			const auto* set = root->set();
			const auto* clear = root->clear();
			const auto* events = root->events();
			if (subjects != nullptr && subjects->size() > CL::kMaxMocapSubjects)
				return ParseError::StringTooLong;
			const size_t items = (set ? set->size() : 0) + (clear ? clear->size() : 0) + (events ? events->size() : 0);
			if (items > CL::kMaxItemsPerMessage)
				return ParseError::TooManyItems;

			if (subjects != nullptr)
			{
				for (flatbuffers::uoffset_t k = 0; k < subjects->size(); ++k)
					m.mocap_subjects.push_back(ReadString(subjects->Get(k)));
			}
			if (set != nullptr)
			{
				for (flatbuffers::uoffset_t k = 0; k < set->size(); ++k)
				{
					const CD::Entry* e = set->Get(k);
					Entry entry;
					entry.key = ReadString(e->key());
					entry.target = ReadString(e->target());
					entry.version = e->version();
					const ReadResult read = ReadValue(*e, entry.value);
					if (read == ReadResult::Bad)
						return ParseError::BadValue;
					entry.supported = read == ReadResult::Ok;
					m.set.push_back(std::move(entry));
				}
			}
			if (clear != nullptr)
			{
				for (flatbuffers::uoffset_t k = 0; k < clear->size(); ++k)
				{
					const CD::Clear* c = clear->Get(k);
					Clear item;
					item.key = ReadString(c->key());
					item.target = ReadString(c->target());
					item.version = c->version();
					m.clear.push_back(std::move(item));
				}
			}
			if (events != nullptr)
			{
				for (flatbuffers::uoffset_t k = 0; k < events->size(); ++k)
				{
					const CD::Event* e = events->Get(k);
					Event ev;
					ev.event_id = e->event_id();
					ev.name = ReadString(e->name());
					ev.target = ReadString(e->target());
					ev.ttl_ms = e->ttl_ms();
					ev.time_us = e->time_us();
					const ReadResult read = ReadValue(*e, ev.value);
					if (read == ReadResult::Bad)
						return ParseError::BadValue;
					ev.supported = read == ReadResult::Ok;
					m.events.push_back(std::move(ev));
				}
			}

			const ParseError error = Validate(m);
			if (error != ParseError::None)
				return error;
			out = std::move(m);
			return ParseError::None;
		}

		// ------------------------------------------------------------------
		// Publisher
		// ------------------------------------------------------------------

		const char* ToString(PublishResult result)
		{
			switch (result)
			{
			case PublishResult::Ok: return "ok";
			case PublishResult::NotRunning: return "the control publisher is not running";
			case PublishResult::Invalid: return "invalid name, target or value";
			case PublishResult::TooLarge: return "the item does not fit one control message";
			case PublishResult::TooManyKeys: return "the control value table is full";
			}
			return "unknown";
		}

		namespace
		{
			// Longest prefix of `text` that is at most maxBytes and ends on a
			// UTF-8 character boundary.
			std::string TruncateUtf8(const std::string& text, size_t maxBytes)
			{
				if (text.size() <= maxBytes)
					return text;
				size_t n = maxBytes;
				while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
					--n;
				return text.substr(0, n);
			}

			PublisherConfig Clamped(PublisherConfig c)
			{
				c.snapshot_interval_s = std::min(10.0, std::max(0.25, std::isfinite(c.snapshot_interval_s) ? c.snapshot_interval_s : 1.0));
				c.event_redundancy = std::min(5, std::max(1, c.event_redundancy));
				c.max_value_rate_hz = std::min(120.0, std::max(1.0, std::isfinite(c.max_value_rate_hz) ? c.max_value_rate_hz : 30.0));
				c.default_event_ttl_ms = std::min(CL::kMaxEventTtlMs, std::max<uint32_t>(1, c.default_event_ttl_ms));
				c.max_keys = std::min(CL::kMaxKeysPerSource, std::max<size_t>(1, c.max_keys));
				c.max_live_bytes_per_s = std::min(CL::kPublisherMaxLiveBytesPerS, std::max(double(CL::kMaxEnvelopeBytes), c.max_live_bytes_per_s));
				c.max_snapshot_bytes_per_s = std::min(CL::kPublisherMaxSnapshotBytesPerS, std::max(double(CL::kMaxEnvelopeBytes), c.max_snapshot_bytes_per_s));
				c.retry_window_s = std::min(60.0, std::max(0.0, std::isfinite(c.retry_window_s) ? c.retry_window_s : 2.0));
				return c;
			}

			// Largest seq and version values, so a size check reserves the
			// widest encoding of every scalar.
			constexpr uint64_t kWideU64 = std::numeric_limits<uint64_t>::max();
		}

		ControlPublisher::ControlPublisher(std::string sourceId, std::string sourceName, const PublisherConfig& config)
			: mSourceId(std::move(sourceId))
			, mSourceName(TruncateUtf8(sourceName, CL::kMaxSourceNameBytes))
			, mConfig(Clamped(config))
		{
			mValidSource = CheckText(mSourceId, CL::kMaxSourceIdBytes, true) == ParseError::None && IsValidUtf8(mSourceName);
		}

		void ControlPublisher::SetConfig(const PublisherConfig& config)
		{
			mConfig = Clamped(config);
		}

		void ControlPublisher::SetMocapSubjects(std::vector<std::string> subjects)
		{
			mMocapSubjects.clear();
			size_t total = 0;
			for (std::string& s : subjects)
			{
				if (mMocapSubjects.size() >= CL::kMaxMocapSubjects)
					break;
				if (s.empty() || s.size() > CL::kMaxMocapSubjectBytes || !IsValidUtf8(s))
					continue;
				if (total + s.size() > CL::kMaxMocapSubjectsTotalBytes)
					break;
				total += s.size();
				mMocapSubjects.push_back(std::move(s));
			}
		}

		void ControlPublisher::Start(uint32_t sessionEpoch)
		{
			mEpoch = std::max(sessionEpoch, mEpoch + 1);
			if (mEpoch == 0)
				mEpoch = 1;
			mRunning = true;
			mHadKeysThisEpoch = !mTable.empty();
			mSnapshotRequested = true;
			mSnapshot = PendingSnapshot();
		}

		void ControlPublisher::Stop()
		{
			mRunning = false;
			mPendingEvents.clear();
			mRetries.clear();
			mSnapshot = PendingSnapshot();
			mSnapshotRequested = false;
		}

		Message ControlPublisher::MakeHeader(uint64_t senderTimeUs, uint64_t wallclockUs) const
		{
			Message m;
			m.source_id = mSourceId;
			m.source_name = mSourceName;
			m.epoch = mEpoch == 0 ? 1 : mEpoch;
			m.sender_time_us = senderTimeUs;
			m.tx_wallclock_us = wallclockUs;
			m.mocap_subjects = mMocapSubjects;
			return m;
		}

		bool ControlPublisher::FitsAlone(const Message& withOneItem) const
		{
			// Encoded with the widest scalars and snapshot fields set, so an
			// item that fits here fits any live message or snapshot part.
			Message m = withOneItem;
			m.seq = kWideU64;
			m.epoch = std::numeric_limits<uint32_t>::max();
			m.sender_time_us = kWideU64;
			m.tx_wallclock_us = kWideU64;
			if (m.events.empty() && m.clear.empty())
			{
				m.snapshot_id = std::numeric_limits<uint32_t>::max();
				m.snapshot_parts = static_cast<uint16_t>(CL::kMaxSnapshotParts);
				m.snapshot_part = m.snapshot_parts - 1;
				m.snapshot_seq = kWideU64;
			}
			for (Entry& e : m.set) e.version = kWideU64;
			for (Clear& c : m.clear) c.version = kWideU64;
			for (Event& ev : m.events) { ev.event_id = kWideU64; ev.time_us = kWideU64; }
			std::vector<uint8_t> bytes;
			return SerializeMessage(m, bytes);
		}

		PublishResult ControlPublisher::SetValue(const std::string& key, const std::string& target, const Value& value)
		{
			if (!mValidSource || CheckText(key, CL::kMaxKeyBytes, true) != ParseError::None
				|| CheckText(target, CL::kMaxTargetBytes, false) != ParseError::None || CheckValue(value) != ParseError::None)
				return PublishResult::Invalid;

			const SlotKey slotKey(key, target);
			auto it = mTable.find(slotKey);
			if (it == mTable.end())
			{
				if (mTable.size() >= mConfig.max_keys)
					return PublishResult::TooManyKeys;
				Message probe = MakeHeader(0, 0);
				Entry entry;
				entry.key = key;
				entry.target = target;
				entry.value = value;
				entry.version = 1;
				probe.set.push_back(entry);
				if (!FitsAlone(probe))
					return PublishResult::TooLarge;
				it = mTable.emplace(slotKey, Slot()).first;
			}
			else
			{
				if (it->second.value == value)
					return PublishResult::Ok;
				if (it->second.value.type != value.type || value.type == ValueType::String || value.type == ValueType::Name
					|| value.type == ValueType::Bytes)
				{
					Message probe = MakeHeader(0, 0);
					Entry entry;
					entry.key = key;
					entry.target = target;
					entry.value = value;
					entry.version = 1;
					probe.set.push_back(entry);
					if (!FitsAlone(probe))
						return PublishResult::TooLarge;
				}
			}

			it->second.value = value;
			it->second.dirty = true;
			mPendingClears.erase(slotKey);
			if (mRunning)
				mHadKeysThisEpoch = true;
			return PublishResult::Ok;
		}

		PublishResult ControlPublisher::ClearValue(const std::string& key, const std::string& target)
		{
			const SlotKey slotKey(key, target);
			const auto it = mTable.find(slotKey);
			if (it == mTable.end())
				return PublishResult::Ok;
			const bool wasSent = it->second.version != 0;
			mTable.erase(it);
			if (wasSent)
				mPendingClears[slotKey] = true;
			return PublishResult::Ok;
		}

		void ControlPublisher::ClearAll()
		{
			for (const auto& kv : mTable)
			{
				if (kv.second.version != 0)
					mPendingClears[kv.first] = true;
			}
			mTable.clear();
		}

		PublishResult ControlPublisher::FireEvent(const std::string& name, const std::string& target, const Value& value,
			uint64_t senderTimeUs, uint32_t ttlMs)
		{
			if (!mRunning)
				return PublishResult::NotRunning;
			if (!mValidSource || CheckText(name, CL::kMaxKeyBytes, true) != ParseError::None
				|| CheckText(target, CL::kMaxTargetBytes, false) != ParseError::None || CheckValue(value) != ParseError::None)
				return PublishResult::Invalid;

			Event ev;
			ev.name = name;
			ev.target = target;
			ev.value = value;
			ev.ttl_ms = ttlMs == 0 ? mConfig.default_event_ttl_ms : std::min(ttlMs, CL::kMaxEventTtlMs);
			ev.time_us = senderTimeUs;

			Message probe = MakeHeader(0, 0);
			probe.events.push_back(ev);
			if (!FitsAlone(probe))
				return PublishResult::TooLarge;

			ev.event_id = mNextEventId++;
			PendingEvent pending;
			pending.event = std::move(ev);
			pending.copies_left = mConfig.event_redundancy;
			mPendingEvents.push_back(std::move(pending));
			return PublishResult::Ok;
		}

		const Value* ControlPublisher::FindValue(const std::string& key, const std::string& target) const
		{
			const auto it = mTable.find(SlotKey(key, target));
			return it == mTable.end() ? nullptr : &it->second.value;
		}

		bool ControlPublisher::Spend(Budget& budget, double ratePerS, size_t bytes, double nowS, bool commit)
		{
			if (!budget.primed)
			{
				budget.tokens = ratePerS;
				budget.last_s = nowS;
				budget.primed = true;
			}
			else if (nowS > budget.last_s)
			{
				budget.tokens = std::min(ratePerS, budget.tokens + (nowS - budget.last_s) * ratePerS);
				budget.last_s = nowS;
			}
			if (budget.tokens < static_cast<double>(bytes))
				return false;
			if (commit)
				budget.tokens -= static_cast<double>(bytes);
			return true;
		}

		void ControlPublisher::EmitLive(double nowS, uint64_t senderTimeUs, uint64_t wallclockUs, std::vector<OutgoingMessage>& out)
		{
			// Items in priority order: clears, events, then due value changes.
			enum class Kind { Clear, Event, Value };
			struct Item
			{
				Kind kind;
				SlotKey key;
				size_t eventIndex;
			};
			std::vector<Item> items;
			for (const auto& kv : mPendingClears)
				items.push_back(Item{ Kind::Clear, kv.first, 0 });
			for (size_t k = 0; k < mPendingEvents.size(); ++k)
				items.push_back(Item{ Kind::Event, SlotKey(), k });
			// 1 % slack: at 60 Hz, two ticks accumulated in floating point can
			// land just under 1/30 s, which would halve a 30 Hz limit.
			const double minGapS = 0.99 / mConfig.max_value_rate_hz;
			for (const auto& kv : mTable)
			{
				if (kv.second.dirty && nowS - kv.second.last_sent_s >= minGapS)
					items.push_back(Item{ Kind::Value, kv.first, 0 });
			}
			if (items.empty())
				return;

			auto addItem = [&](Message& m, const Item& item)
			{
				switch (item.kind)
				{
				case Kind::Clear:
				{
					Clear c;
					c.key = item.key.first;
					c.target = item.key.second;
					c.version = m.seq;
					m.clear.push_back(std::move(c));
					break;
				}
				case Kind::Event:
					m.events.push_back(mPendingEvents[item.eventIndex].event);
					break;
				case Kind::Value:
				{
					Entry e;
					e.key = item.key.first;
					e.target = item.key.second;
					e.value = mTable[item.key].value;
					e.version = m.seq;
					m.set.push_back(std::move(e));
					break;
				}
				}
			};
			auto removeLast = [](Message& m, const Item& item)
			{
				if (item.kind == Kind::Clear) m.clear.pop_back();
				else if (item.kind == Kind::Event) m.events.pop_back();
				else m.set.pop_back();
			};

			// Pack greedily; commit each message only if the live budget has
			// room for it. Items left over stay pending for a later tick.
			std::vector<bool> sent(items.size(), false);
			size_t next = 0;
			while (next < items.size())
			{
				Message m = MakeHeader(senderTimeUs, wallclockUs);
				m.seq = mLastSeq + 1;
				std::vector<uint8_t> bytes;
				std::vector<size_t> inMessage;
				while (next < items.size())
				{
					addItem(m, items[next]);
					std::vector<uint8_t> candidate;
					if (m.ItemCount() <= CL::kMaxItemsPerMessage && SerializeMessage(m, candidate))
					{
						bytes.swap(candidate);
						inMessage.push_back(next);
						++next;
						continue;
					}
					removeLast(m, items[next]);
					if (inMessage.empty())
					{
						// Cannot happen for items admitted by FitsAlone(); skip
						// rather than loop forever.
						sent[next] = true;
						++next;
					}
					break;
				}
				if (inMessage.empty())
					continue;
				if (!Spend(mLiveBudget, mConfig.max_live_bytes_per_s, bytes.size(), nowS, true))
					break;

				// Committed: the message goes out with seq m.seq, and each value
				// in it takes that seq as its version.
				mLastSeq = m.seq;
				for (size_t index : inMessage)
				{
					sent[index] = true;
					const Item& item = items[index];
					if (item.kind == Kind::Value)
					{
						Slot& slot = mTable[item.key];
						slot.version = m.seq;
						slot.dirty = false;
						slot.last_sent_s = nowS;
					}
				}
				OutgoingMessage msg;
				msg.bytes = std::move(bytes);
				msg.seq = m.seq;
				out.push_back(std::move(msg));
			}

			// Clears and events that went out. Event indices refer to
			// mPendingEvents as it was above; erase from the back.
			std::vector<size_t> finishedEvents;
			for (size_t k = 0; k < items.size(); ++k)
			{
				if (!sent[k])
					continue;
				const Item& item = items[k];
				if (item.kind == Kind::Clear)
					mPendingClears.erase(item.key);
				else if (item.kind == Kind::Event && --mPendingEvents[item.eventIndex].copies_left <= 0)
					finishedEvents.push_back(item.eventIndex);
			}
			for (auto it = finishedEvents.rbegin(); it != finishedEvents.rend(); ++it)
				mPendingEvents.erase(mPendingEvents.begin() + static_cast<std::ptrdiff_t>(*it));
		}

		void ControlPublisher::BuildSnapshot(double nowS, uint64_t senderTimeUs, uint64_t wallclockUs)
		{
			mSnapshotRequested = false;
			mLastSnapshotS = nowS;
			mSnapshot = PendingSnapshot();
			if (!mHadKeysThisEpoch)
				return;

			uint32_t id = mNextSnapshotId++;
			if (id == 0)
				id = mNextSnapshotId++;
			const uint64_t snapshotSeq = ++mLastSeq; // capture point; every version in the table is at or below it

			// The snapshot carries every key's current value. A value not yet
			// sent live (new, or changed and waiting on the live budget or the
			// per-key rate) takes the capture point as its version and is no
			// longer pending: the snapshot delivers it. Otherwise a sender
			// whose live budget never reaches some keys would never deliver
			// them at all.
			std::vector<Entry> entries;
			for (auto& kv : mTable)
			{
				Slot& slot = kv.second;
				if (slot.dirty || slot.version == 0)
				{
					slot.version = snapshotSeq;
					slot.dirty = false;
				}
				Entry e;
				e.key = kv.first.first;
				e.target = kv.first.second;
				e.value = slot.value;
				e.version = slot.version;
				entries.push_back(std::move(e));
			}

			// Pack with placeholder part fields at their widest, then stamp the
			// real ones. An empty table still sends one (empty) part, so
			// receivers learn every earlier key is gone.
			std::vector<Message> parts;
			Message current = MakeHeader(senderTimeUs, wallclockUs);
			current.snapshot_id = id;
			current.snapshot_seq = snapshotSeq;
			current.snapshot_parts = static_cast<uint16_t>(CL::kMaxSnapshotParts);
			current.snapshot_part = current.snapshot_parts - 1;
			current.seq = kWideU64;
			for (Entry& e : entries)
			{
				current.set.push_back(e);
				std::vector<uint8_t> candidate;
				if (current.set.size() <= CL::kMaxItemsPerMessage && SerializeMessage(current, candidate))
					continue;
				current.set.pop_back();
				parts.push_back(current);
				current.set.clear();
				current.set.push_back(std::move(e));
			}
			parts.push_back(current);

			const size_t count = std::min(parts.size(), CL::kMaxSnapshotParts);
			const double spacingS = count > 1 ? (mConfig.snapshot_interval_s * 0.5) / static_cast<double>(count) : 0.0;
			for (size_t k = 0; k < count; ++k)
			{
				Message& part = parts[k];
				part.seq = ++mLastSeq;
				part.snapshot_part = static_cast<uint16_t>(k);
				part.snapshot_parts = static_cast<uint16_t>(count);
				OutgoingMessage msg;
				msg.seq = part.seq;
				msg.snapshot = true;
				if (!SerializeMessage(part, msg.bytes))
					continue;
				mSnapshot.parts.push_back(std::move(msg));
				mSnapshot.due_s.push_back(nowS + spacingS * static_cast<double>(k));
			}
		}

		void ControlPublisher::Tick(double nowS, uint64_t senderTimeUs, uint64_t wallclockUs, std::vector<OutgoingMessage>& out)
		{
			if (!mRunning)
				return;

			// 1. Live changes first, so the snapshot below captures the
			//    versions they were just given.
			EmitLive(nowS, senderTimeUs, wallclockUs, out);

			// 2. Snapshot: on request, or every interval once this epoch has
			//    had keys. A new one is not started while one is still going
			//    out, unless requested.
			const bool snapshotInFlight = mSnapshot.next < mSnapshot.parts.size();
			const bool intervalDue = mHadKeysThisEpoch && (nowS - mLastSnapshotS) >= mConfig.snapshot_interval_s;
			if (mSnapshotRequested || (intervalDue && !snapshotInFlight))
				BuildSnapshot(nowS, senderTimeUs, wallclockUs);

			// 3. Snapshot parts that are due, within the snapshot budget.
			while (mSnapshot.next < mSnapshot.parts.size() && mSnapshot.due_s[mSnapshot.next] <= nowS)
			{
				OutgoingMessage& part = mSnapshot.parts[mSnapshot.next];
				if (!Spend(mSnapshotBudget, mConfig.max_snapshot_bytes_per_s, part.bytes.size(), nowS, true))
					break;
				out.push_back(std::move(part));
				++mSnapshot.next;
			}

			// 4. Retries, last, within whatever live budget is left.
			while (!mRetries.empty())
			{
				Retry& retry = mRetries.front();
				if (nowS - retry.first_refused_s > mConfig.retry_window_s)
				{
					mRetries.pop_front();
					continue;
				}
				if (!Spend(mLiveBudget, mConfig.max_live_bytes_per_s, retry.message.bytes.size(), nowS, true))
					break;
				out.push_back(retry.message);
				mRetries.pop_front();
			}
		}

		void ControlPublisher::OnSendRefused(const OutgoingMessage& message, double nowS)
		{
			if (!mRunning || message.snapshot)
				return; // the next snapshot repairs lost parts
			Retry retry;
			retry.message = message;
			retry.first_refused_s = nowS;
			for (const Retry& existing : mRetries)
			{
				if (existing.message.seq == message.seq)
				{
					retry.first_refused_s = existing.first_refused_s;
					break;
				}
			}
			mRetries.push_back(std::move(retry));
			while (mRetries.size() > mConfig.max_retry_messages)
				mRetries.pop_front();
		}

		// ------------------------------------------------------------------
		// Receiver
		// ------------------------------------------------------------------

		namespace
		{
			//! (epoch, version) ordering: a newer epoch always wins.
			bool Newer(uint32_t epochA, uint64_t versionA, uint32_t epochB, uint64_t versionB)
			{
				return epochA > epochB || (epochA == epochB && versionA > versionB);
			}

			bool AtOrBelow(uint32_t epochA, uint64_t versionA, uint32_t epochB, uint64_t versionB)
			{
				return !Newer(epochA, versionA, epochB, versionB);
			}
		}

		ControlReceiver::ControlReceiver(const ReceiverConfig& config)
			: mConfig(config)
		{
		}

		void ControlReceiver::SetConfig(const ReceiverConfig& config)
		{
			mConfig = config;
		}

		bool ControlReceiver::Allowed(const std::string& name) const
		{
			if (mConfig.allow_prefixes.empty())
				return true;
			for (const std::string& prefix : mConfig.allow_prefixes)
			{
				if (name.compare(0, prefix.size(), prefix) == 0)
					return true;
			}
			return false;
		}

		bool ControlReceiver::TakeTokens(Bucket& bucket, double ratePerS, size_t bytes, double nowS)
		{
			if (!bucket.primed)
			{
				bucket.tokens = ratePerS;
				bucket.last_s = nowS;
				bucket.primed = true;
			}
			else if (nowS > bucket.last_s)
			{
				bucket.tokens = std::min(ratePerS, bucket.tokens + (nowS - bucket.last_s) * ratePerS);
				bucket.last_s = nowS;
			}
			if (bucket.tokens < static_cast<double>(bytes))
				return false;
			bucket.tokens -= static_cast<double>(bytes);
			return true;
		}

		ParseError ControlReceiver::Submit(const uint8_t* data, size_t len, double nowS, std::vector<Change>& out)
		{
			Message message;
			const ParseError error = ParseMessage(data, len, message);
			if (error != ParseError::None)
			{
				++mStats.rejected_parse;
				return error;
			}
			Apply(message, len, nowS, out);
			return ParseError::None;
		}

		void ControlReceiver::Apply(const Message& message, size_t wireBytes, double nowS, std::vector<Change>& out)
		{
			auto it = mSources.find(message.source_id);
			if (it == mSources.end())
			{
				if (mSources.size() >= mConfig.max_sources)
				{
					++mStats.dropped_source_cap;
					return;
				}
				it = mSources.emplace(message.source_id, Source()).first;
			}
			Source& source = it->second;
			const std::string& sourceId = it->first;

			Bucket& bucket = message.IsSnapshot() ? source.snapshot : source.live;
			const double rate = message.IsSnapshot() ? mConfig.max_snapshot_bytes_per_s : mConfig.max_live_bytes_per_s;
			if (!TakeTokens(bucket, rate, wireBytes, nowS))
			{
				++mStats.dropped_rate;
				return;
			}

			if (source.has_epoch && message.epoch < source.epoch)
			{
				++mStats.dropped_old_epoch;
				return;
			}
			if (!source.has_epoch || message.epoch > source.epoch)
			{
				// A new session of the same source. Values are kept: the first
				// complete snapshot of the new epoch reconciles them, so a
				// sender restart does not make receivers flicker (ADR 0011
				// item 9, Epoch). Event identity and the TTL clock restart.
				source.epoch = message.epoch;
				source.has_epoch = true;
				source.seen_events.clear();
				source.seen_order.clear();
				source.newest_sender_time_us = 0;
				if (source.assembling)
				{
					++mStats.snapshots_discarded;
					source.assembling = false;
				}
			}

			source.name = message.source_name;
			source.last_seen_s = nowS;
			if (!message.mocap_subjects.empty())
				source.mocap_subjects = message.mocap_subjects;
			source.newest_sender_time_us = std::max(source.newest_sender_time_us, message.sender_time_us);
			++mStats.messages_accepted;

			for (const Entry& entry : message.set)
				ApplyEntry(source, sourceId, message, entry, out);
			for (const Clear& clear : message.clear)
				ApplyClear(source, sourceId, message, clear, out);
			for (const Event& event : message.events)
				ApplyEvent(source, sourceId, message, event, out);
			if (message.IsSnapshot())
				TrackSnapshot(source, sourceId, message, nowS, out);
		}

		void ControlReceiver::ApplyEntry(Source& source, const std::string& sourceId, const Message& message, const Entry& entry, std::vector<Change>& out)
		{
			if (!Allowed(entry.key))
			{
				++mStats.items_not_allowed;
				return;
			}
			if (!entry.supported)
			{
				++mStats.items_unsupported;
				return;
			}
			const SlotKey key(entry.key, entry.target);
			const auto tomb = source.tombstones.find(key);
			if (tomb != source.tombstones.end() && AtOrBelow(message.epoch, entry.version, tomb->second.epoch, tomb->second.version))
			{
				++mStats.items_stale;
				return;
			}

			auto slot = source.values.find(key);
			if (slot != source.values.end())
			{
				const bool newer = Newer(message.epoch, entry.version, slot->second.epoch, slot->second.version);
				if (!newer)
				{
					if (message.epoch < slot->second.epoch || entry.version < slot->second.version)
						++mStats.items_stale;
					return;
				}
				const bool changed = slot->second.value != entry.value;
				slot->second.value = entry.value;
				slot->second.epoch = message.epoch;
				slot->second.version = entry.version;
				if (!changed)
					return;
			}
			else
			{
				// The floor catches a late live set of a key cleared before the
				// last complete snapshot. A snapshot entry instead says the key
				// existed at its own capture point, so compare that: its
				// version may legitimately be older than the floor (a key this
				// receiver could not hold until now, e.g. after a key-cap drop).
				const uint64_t asOf = message.IsSnapshot() ? message.snapshot_seq : entry.version;
				if (tomb == source.tombstones.end() && (source.floor_epoch != 0 || source.floor_seq != 0)
					&& AtOrBelow(message.epoch, asOf, source.floor_epoch, source.floor_seq))
				{
					++mStats.items_stale;
					return;
				}
				const size_t used = source.values.size() + source.tombstones.size() - (tomb != source.tombstones.end() ? 1 : 0);
				if (used >= mConfig.max_keys_per_source)
				{
					++mStats.items_key_cap;
					return;
				}
				Slot fresh;
				fresh.value = entry.value;
				fresh.epoch = message.epoch;
				fresh.version = entry.version;
				source.values.emplace(key, std::move(fresh));
			}
			if (tomb != source.tombstones.end())
				source.tombstones.erase(tomb);

			Change change;
			change.kind = Change::Kind::ValueChanged;
			change.source_id = sourceId;
			change.source_name = source.name;
			change.name = entry.key;
			change.target = entry.target;
			change.value = entry.value;
			change.epoch = message.epoch;
			change.seq = message.seq;
			change.sender_time_us = message.sender_time_us;
			change.version = entry.version;
			out.push_back(std::move(change));
		}

		void ControlReceiver::ApplyClear(Source& source, const std::string& sourceId, const Message& message, const Clear& clear, std::vector<Change>& out)
		{
			if (!Allowed(clear.key))
			{
				++mStats.items_not_allowed;
				return;
			}
			const SlotKey key(clear.key, clear.target);

			const auto slot = source.values.find(key);
			if (slot != source.values.end())
			{
				if (!Newer(message.epoch, clear.version, slot->second.epoch, slot->second.version))
				{
					++mStats.items_stale;
					return;
				}
				source.values.erase(slot);
				Change change;
				change.kind = Change::Kind::ValueCleared;
				change.source_id = sourceId;
				change.source_name = source.name;
				change.name = clear.key;
				change.target = clear.target;
				change.epoch = message.epoch;
				change.seq = message.seq;
				change.sender_time_us = message.sender_time_us;
				change.version = clear.version;
				out.push_back(std::move(change));
			}

			// Remember the clear, so an older set that arrives later is
			// rejected. Under the key cap, the slot just freed makes room.
			auto tomb = source.tombstones.find(key);
			if (tomb != source.tombstones.end())
			{
				if (Newer(message.epoch, clear.version, tomb->second.epoch, tomb->second.version))
				{
					tomb->second.epoch = message.epoch;
					tomb->second.version = clear.version;
				}
				return;
			}
			if (source.values.size() + source.tombstones.size() >= mConfig.max_keys_per_source)
			{
				++mStats.items_key_cap;
				return;
			}
			Tombstone t;
			t.epoch = message.epoch;
			t.version = clear.version;
			source.tombstones.emplace(key, t);
		}

		void ControlReceiver::ApplyEvent(Source& source, const std::string& sourceId, const Message& message, const Event& event, std::vector<Change>& out)
		{
			if (!Allowed(event.name))
			{
				++mStats.items_not_allowed;
				return;
			}
			if (!event.supported)
			{
				++mStats.items_unsupported;
				return;
			}
			if (source.seen_events.count(event.event_id) != 0)
			{
				++mStats.events_duplicate;
				return;
			}
			source.seen_events.insert(event.event_id);
			source.seen_order.push_back(event.event_id);
			while (source.seen_order.size() > std::max<size_t>(1, mConfig.dedupe_window))
			{
				source.seen_events.erase(source.seen_order.front());
				source.seen_order.pop_front();
			}

			// TTL on the sender's own clock: no cross-host clock sync needed.
			const uint64_t firedUs = event.time_us != 0 ? event.time_us : message.sender_time_us;
			const uint64_t ttlUs = static_cast<uint64_t>(event.ttl_ms) * 1000u;
			if (firedUs + ttlUs < source.newest_sender_time_us)
			{
				++mStats.events_expired;
				return;
			}

			Change change;
			change.kind = Change::Kind::Event;
			change.source_id = sourceId;
			change.source_name = source.name;
			change.name = event.name;
			change.target = event.target;
			change.value = event.value;
			change.epoch = message.epoch;
			change.seq = message.seq;
			change.sender_time_us = firedUs;
			change.event_id = event.event_id;
			out.push_back(std::move(change));
		}

		void ControlReceiver::TrackSnapshot(Source& source, const std::string& sourceId, const Message& message, double nowS, std::vector<Change>& out)
		{
			Assembly& a = source.assembly;
			const bool newer = !source.assembling || message.epoch > a.epoch || (message.epoch == a.epoch && message.snapshot_id > a.id);
			if (newer)
			{
				if (source.assembling)
					++mStats.snapshots_discarded;
				a = Assembly();
				a.id = message.snapshot_id;
				a.epoch = message.epoch;
				a.parts = message.snapshot_parts;
				a.snapshot_seq = message.snapshot_seq;
				a.have.assign(message.snapshot_parts, false);
				a.part_first.assign(message.snapshot_parts, SlotKey());
				a.part_last.assign(message.snapshot_parts, SlotKey());
				source.assembling = true;
			}
			else if (message.snapshot_id != a.id || message.epoch != a.epoch)
			{
				return; // a part of an older snapshot: its entries were applied by version
			}
			else if (message.snapshot_parts != a.parts || message.snapshot_seq != a.snapshot_seq)
			{
				++mStats.snapshots_discarded;
				source.assembling = false;
				return;
			}

			a.last_part_s = nowS;
			const size_t p = message.snapshot_part;
			if (a.have[p])
				return;
			a.have[p] = true;
			++a.received;

			// Repair lost clears from this part alone: a key absent from the
			// part's range was absent at capture. Gaps between parts are
			// covered once both neighbours have arrived, the ends by the
			// first and last parts. A lost part delays only its own range.
			if (message.set.empty())
			{
				RemoveAbsent(source, sourceId, nullptr, false, nullptr, false, nullptr, out); // empty table
			}
			else
			{
				std::set<SlotKey> present;
				for (const Entry& entry : message.set)
					present.insert(SlotKey(entry.key, entry.target));
				a.part_first[p] = *present.begin();
				a.part_last[p] = *present.rbegin();
				const SlotKey first = a.part_first[p];
				const SlotKey last = a.part_last[p];

				RemoveAbsent(source, sourceId, &first, true, &last, true, &present, out);
				if (p == 0)
					RemoveAbsent(source, sourceId, nullptr, false, &first, false, nullptr, out);
				else if (a.have[p - 1])
					RemoveAbsent(source, sourceId, &a.part_last[p - 1], false, &first, false, nullptr, out);
				if (p + 1 == a.parts)
					RemoveAbsent(source, sourceId, &last, false, nullptr, false, nullptr, out);
				else if (a.have[p + 1])
					RemoveAbsent(source, sourceId, &last, false, &a.part_first[p + 1], false, nullptr, out);
			}

			if (a.received == a.parts)
			{
				CompleteSnapshot(source);
				source.assembling = false;
				++mStats.snapshots_completed;
			}
		}

		void ControlReceiver::RemoveAbsent(Source& source, const std::string& sourceId, const SlotKey* low, bool lowInclusive,
			const SlotKey* high, bool highInclusive, const std::set<SlotKey>* present, std::vector<Change>& out)
		{
			const Assembly& a = source.assembly;
			auto it = low == nullptr ? source.values.begin()
				: (lowInclusive ? source.values.lower_bound(*low) : source.values.upper_bound(*low));
			while (it != source.values.end())
			{
				if (high != nullptr && (highInclusive ? (*high < it->first) : !(it->first < *high)))
					break;
				const bool keep = (present != nullptr && present->count(it->first) != 0)
					|| !AtOrBelow(it->second.epoch, it->second.version, a.epoch, a.snapshot_seq);
				if (keep)
				{
					++it;
					continue;
				}
				Change change;
				change.kind = Change::Kind::ValueCleared;
				change.source_id = sourceId;
				change.source_name = source.name;
				change.name = it->first.first;
				change.target = it->first.second;
				change.epoch = a.epoch;
				change.seq = a.snapshot_seq;
				change.sender_time_us = source.newest_sender_time_us;
				change.version = a.snapshot_seq;
				out.push_back(std::move(change));
				it = source.values.erase(it);
			}
		}

		void ControlReceiver::CompleteSnapshot(Source& source)
		{
			// Absent keys were already removed part by part. What needs the
			// whole snapshot is forgetting tombstones: only then is every key
			// at or below the capture point accounted for, and the floor
			// takes their place.
			const Assembly& a = source.assembly;

			// Tombstones at or below the capture point are now covered by the
			// floor; later ones are kept.
			for (auto it = source.tombstones.begin(); it != source.tombstones.end();)
			{
				if (AtOrBelow(it->second.epoch, it->second.version, a.epoch, a.snapshot_seq))
					it = source.tombstones.erase(it);
				else
					++it;
			}
			if (Newer(a.epoch, a.snapshot_seq, source.floor_epoch, source.floor_seq))
			{
				source.floor_epoch = a.epoch;
				source.floor_seq = a.snapshot_seq;
			}
		}

		void ControlReceiver::ReportCleared(const Source& source, const std::string& sourceId, std::vector<Change>& out)
		{
			for (const auto& kv : source.values)
			{
				Change change;
				change.kind = Change::Kind::ValueCleared;
				change.source_id = sourceId;
				change.source_name = source.name;
				change.name = kv.first.first;
				change.target = kv.first.second;
				change.epoch = source.epoch;
				out.push_back(std::move(change));
			}
		}

		void ControlReceiver::Tick(double nowS, std::vector<Change>& out)
		{
			for (auto it = mSources.begin(); it != mSources.end();)
			{
				Source& source = it->second;
				if (source.assembling && nowS - source.assembly.last_part_s > mConfig.incomplete_snapshot_timeout_s)
				{
					source.assembling = false;
					++mStats.snapshots_discarded;
				}
				if (nowS - source.last_seen_s > mConfig.source_idle_timeout_s)
				{
					ReportCleared(source, it->first, out);
					++mStats.sources_pruned;
					it = mSources.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		const Value* ControlReceiver::FindValue(const std::string& sourceId, const std::string& key, const std::string& target) const
		{
			const auto source = mSources.find(sourceId);
			if (source == mSources.end())
				return nullptr;
			const auto slot = source->second.values.find(SlotKey(key, target));
			return slot == source->second.values.end() ? nullptr : &slot->second.value;
		}

		std::vector<std::pair<SlotKey, Value>> ControlReceiver::GetValues(const std::string& sourceId) const
		{
			std::vector<std::pair<SlotKey, Value>> values;
			const auto source = mSources.find(sourceId);
			if (source == mSources.end())
				return values;
			for (const auto& kv : source->second.values)
				values.emplace_back(kv.first, kv.second.value);
			return values;
		}

		const std::vector<std::string>* ControlReceiver::FindMocapSubjects(const std::string& sourceId) const
		{
			const auto source = mSources.find(sourceId);
			return source == mSources.end() ? nullptr : &source->second.mocap_subjects;
		}

		void ControlReceiver::Reset()
		{
			mSources.clear();
		}

		// ------------------------------------------------------------------
		// Aligner
		// ------------------------------------------------------------------

		void ControlAligner::Push(Change&& change, double nowS)
		{
			Held held;
			held.change = std::move(change);
			held.pushed_s = nowS;
			std::deque<Held>& queue = mQueues[held.change.source_id];
			queue.push_back(std::move(held));
		}

		void ControlAligner::ReleaseQueue(std::deque<Held>& queue, bool aligned, uint64_t presentingUs, double nowS, std::vector<Change>& out)
		{
			while (!queue.empty())
			{
				Held& head = queue.front();
				if (!aligned)
					++mStats.released_unaligned;
				else if (head.change.sender_time_us <= presentingUs)
					++mStats.released_aligned;
				else if (nowS - head.pushed_s >= mMaxHoldS)
					++mStats.released_late;
				else
					break;
				out.push_back(std::move(head.change));
				queue.pop_front();
			}
		}

		void ControlAligner::Prune()
		{
			for (auto it = mQueues.begin(); it != mQueues.end();)
			{
				if (it->second.empty())
					it = mQueues.erase(it);
				else
					++it;
			}
		}

		void ControlAligner::Flush(std::vector<Change>& out)
		{
			for (auto& queue : mQueues)
			{
				for (Held& held : queue.second)
					out.push_back(std::move(held.change));
			}
			mQueues.clear();
		}

		size_t ControlAligner::NumHeld() const
		{
			size_t n = 0;
			for (const auto& queue : mQueues)
				n += queue.second.size();
			return n;
		}
	}
}
