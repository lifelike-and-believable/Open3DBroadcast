// Seed corpus and helpers shared by every O3DS core fuzz target. See
// fuzz_support.h for how the pieces fit together.
#include "fuzz_support.h"

#include "o3ds/model.h"
#include "o3ds/predict/quat_math.h"
#include "o3ds/tcp_stream_parser.h"

#include "CRC.h"

#include <algorithm>
#include <cstring>
#include <memory>

using namespace O3DS;

namespace o3ds_fuzz
{
	void (*gOnAssertFailure)() = nullptr;

	namespace
	{
		// Every timestamp passed to Serialize*() is explicit: a timestamp of
		// 0.0 means "use GetTime()", which would make the seeds (and so the
		// replay runner's whole mutation sequence) differ from run to run.
		const double kKeyframeTime = 1.0;
		const double kFrameStep = 0.02;

		Bytes ToBytes(const std::vector<char>& v)
		{
			return Bytes(v.begin(), v.end());
		}

		// Strips the 8-byte flags/CRC header: the parse targets fuzz the
		// payload and recompute the header themselves (see WrapPayload()).
		Bytes Payload(const std::vector<char>& wire)
		{
			if (wire.size() < 8)
				return Bytes();
			return Bytes(wire.begin() + 8, wire.end());
		}

		void AddTrs(Transform* t)
		{
			t->transformOrder.push_back(O3DS::TTranslation);
			t->transformOrder.push_back(O3DS::TRotation);
			t->transformOrder.push_back(O3DS::TScale);
		}

		// Two subjects covering every wire feature the parser handles:
		// a small hierarchy with translation/rotation/scale components and
		// curves ("Actor"), and a node whose transform is a matrix
		// component backed by a matrix vector ("Prop"; CORE-1 is exactly a
		// matrix component *without* that vector).
		void BuildCanonical(SubjectList& list)
		{
			Subject* actor = list.addSubject("Actor");
			actor->mCurveNames = { "Smile", "Blink" };
			actor->mCurveValues = { 0.25f, 0.5f };
			AddTrs(actor->addTransform("Root", -1));
			AddTrs(actor->addTransform("Spine", 0));
			AddTrs(actor->addTransform("Head", 1));
			AddTrs(actor->addTransform("Hand", 1));

			Subject* prop = list.addSubject("Prop");
			Transform* body = prop->addTransform("Body", -1);
			body->transformOrder.push_back(O3DS::TMatrix);
			body->matrices.push_back(TransformMatrix());
		}

		void ApplyMotion(SubjectList& list, double t)
		{
			Subject* actor = list.findSubject("Actor");
			for (size_t i = 0; i < actor->mTransforms.size(); ++i)
			{
				const double k = 1.0 + (double)i;
				actor->mTransforms[i]->translation.value = Vector3d(0.1 * k * t, 0.05 * k * t, -0.02 * t);
				actor->mTransforms[i]->rotation.value = QuatFromAxisAngle(Vector3d(0.0, 0.0, 1.0), 0.3 * k * t);
				actor->mTransforms[i]->scale.value = Vector3d(1.0, 1.0 + 0.01 * t, 1.0);
			}
			actor->mCurveValues[0] = (float)(0.1 * t);
			actor->mCurveValues[1] = (float)(1.0 - 0.1 * t);
		}

		std::vector<char> SerializeKeyframe(SubjectList& list, uint64_t txSeq = 0, uint32_t epoch = 0)
		{
			std::vector<char> out;
			list.Serialize(out, kKeyframeTime, txSeq, txSeq ? 1700000000000000ull + txSeq : 0, epoch);
			return out;
		}

		// A few frames of legacy (predictor_id 0) updates, optionally with
		// D1 quantization on.
		std::vector<std::vector<char>> LegacyUpdates(bool quantized, int frames)
		{
			SubjectList sender;
			BuildCanonical(sender);
			sender.SetDeltaThreshold(1.0e-6);
			if (quantized)
			{
				sender.mQuantizationEnabled = true;
				sender.mQuantRanges.byteRange = 0.05;
				sender.mQuantRanges.halfRange = 1.0;
			}
			SerializeKeyframe(sender); // sets last-sent values and quant anchors

			std::vector<std::vector<char>> out;
			for (int i = 1; i <= frames; ++i)
			{
				const double t = kKeyframeTime + i * kFrameStep;
				ApplyMotion(sender, t);
				size_t count = 0;
				std::vector<char> buf;
				sender.SerializeUpdate(buf, count, t, (uint64_t)i + 1, 1700000000000000ull + (uint64_t)i, 7);
				out.push_back(buf);
			}
			return out;
		}

		// A short residual-coded (C2) stream. keyframeInterval > 0 forces
		// periodic keyframes inside the stream as well as the first one.
		std::vector<std::vector<char>> ResidualUpdates(ResidualPredictorId id, uint32_t keyframeInterval, int frames)
		{
			SubjectList sender;
			BuildCanonical(sender);
			sender.SetDeltaThreshold(1.0e-6);
			sender.findSubject("Actor")->SetResidualEncoder(std::make_unique<ResidualEncoder>(id, keyframeInterval));
			SerializeKeyframe(sender);

			std::vector<std::vector<char>> out;
			for (int i = 1; i <= frames; ++i)
			{
				const double t = kKeyframeTime + i * kFrameStep;
				ApplyMotion(sender, t);
				size_t count = 0;
				std::vector<char> buf;
				sender.SerializeUpdateResidual(buf, count, t, (uint64_t)i + 1, 0, 0);
				out.push_back(buf);
			}
			return out;
		}

		std::vector<char> SmallHierarchyKeyframe()
		{
			SubjectList list;
			Subject* s = list.addSubject("Chain");
			AddTrs(s->addTransform("a", -1));
			AddTrs(s->addTransform("b", 0));
			AddTrs(s->addTransform("c", 1));
			return SerializeKeyframe(list, 42, 3);
		}

		std::vector<Bytes> ParseSeeds()
		{
			std::vector<Bytes> seeds;
			seeds.push_back(Payload(CanonicalKeyframe()));
			seeds.push_back(Payload(SmallHierarchyKeyframe()));
			{
				SubjectList empty;
				seeds.push_back(Payload(SerializeKeyframe(empty)));
			}
			for (const auto& b : LegacyUpdates(false, 1)) seeds.push_back(Payload(b));
			for (const auto& b : LegacyUpdates(true, 2)) seeds.push_back(Payload(b));
			for (const auto& b : ResidualUpdates(ResidualPredictorId::Linear, 0, 2)) seeds.push_back(Payload(b));
			return seeds;
		}

		std::vector<Bytes> ParseUpdateSeeds()
		{
			std::vector<Bytes> seeds;
			for (const auto& b : LegacyUpdates(false, 2)) seeds.push_back(Payload(b));
			for (const auto& b : LegacyUpdates(true, 2)) seeds.push_back(Payload(b));
			for (const auto& b : ResidualUpdates(ResidualPredictorId::Linear, 0, 2)) seeds.push_back(Payload(b));
			return seeds;
		}

		std::vector<Bytes> ResidualSeeds()
		{
			struct Stream { ResidualPredictorId id; uint32_t keyframeInterval; int frames; };
			const Stream streams[] = {
				{ ResidualPredictorId::Hold, 0, 3 },
				{ ResidualPredictorId::Linear, 0, 4 },
				{ ResidualPredictorId::Quadratic, 2, 4 },
			};

			std::vector<Bytes> seeds;
			for (const Stream& s : streams)
			{
				Bytes seed;
				for (const auto& b : ResidualUpdates(s.id, s.keyframeInterval, s.frames))
				{
					Bytes p = Payload(b);
					AppendRecord(seed, p.data(), p.size());
				}
				seeds.push_back(seed);
			}
			return seeds;
		}

		std::vector<Bytes> PeekMetaSeeds()
		{
			std::vector<Bytes> seeds;
			seeds.push_back(ToBytes(CanonicalKeyframe()));
			seeds.push_back(ToBytes(SmallHierarchyKeyframe()));
			for (const auto& b : LegacyUpdates(false, 1)) seeds.push_back(ToBytes(b));
			seeds.push_back(Bytes(8, 0));
			return seeds;
		}

		// One fuzz_udp_reassembly record per datagram: the real 16-byte
		// fragment header, lenMode 0 ("honest" payload length, bytes copied
		// from CanonicalKeyframe()) and the control byte (sender and clock
		// step) - see fuzz_udp_reassembly.cpp.
		void AppendUdpRecord(Bytes& out, uint32_t id, uint32_t seq, uint32_t bufSz, uint32_t fragSize, uint8_t ctl = 0)
		{
			const uint32_t header[4] = { id, seq, bufSz, fragSize };
			const uint8_t* p = reinterpret_cast<const uint8_t*>(header);
			out.insert(out.end(), p, p + sizeof(header));
			out.push_back(0);
			out.push_back(ctl);
		}

		std::vector<Bytes> UdpSeeds()
		{
			const uint32_t bufSz = (uint32_t)CanonicalKeyframe().size();
			const uint32_t fragSize = 128;
			const uint32_t frames = (bufSz + fragSize - 1) / fragSize;

			std::vector<Bytes> seeds;

			Bytes inOrder;
			for (uint32_t s = 0; s < frames; ++s) AppendUdpRecord(inOrder, 1, s, bufSz, fragSize);
			seeds.push_back(inOrder);

			Bytes reversedWithDup;
			for (uint32_t s = frames; s-- > 0;) AppendUdpRecord(reversedWithDup, 2, s, bufSz, fragSize);
			AppendUdpRecord(reversedWithDup, 2, 0, bufSz, fragSize);
			seeds.push_back(reversedWithDup);

			// Two messages in flight at once, fragmented at different sizes.
			const uint32_t otherFragSize = 96;
			const uint32_t otherFrames = (bufSz + otherFragSize - 1) / otherFragSize;
			Bytes interleaved;
			for (uint32_t s = 0; s < std::max(frames, otherFrames); ++s)
			{
				if (s < frames) AppendUdpRecord(interleaved, 3, s, bufSz, fragSize);
				if (s < otherFrames) AppendUdpRecord(interleaved, 4, s, bufSz, otherFragSize);
			}
			seeds.push_back(interleaved);

			Bytes single;
			AppendUdpRecord(single, 5, 0, bufSz, bufSz);
			seeds.push_back(single);

			// The same message id from two senders at once (ctl bits 0-1).
			Bytes twoSources;
			for (uint32_t s = 0; s < frames; ++s)
			{
				AppendUdpRecord(twoSources, 6, s, bufSz, fragSize, 0);
				AppendUdpRecord(twoSources, 6, s, bufSz, fragSize, 1);
			}
			seeds.push_back(twoSources);

			// A message whose middle fragment arrives after the 100 ms
			// timeout (ctl bits 2-7: 63 * 4 = 252 ms clock step), then a
			// complete retransmission under a new id.
			Bytes expired;
			for (uint32_t s = 0; s < frames; ++s)
				AppendUdpRecord(expired, 7, s, bufSz, fragSize, s == frames / 2 ? (uint8_t)(63 << 2) : 0);
			for (uint32_t s = 0; s < frames; ++s) AppendUdpRecord(expired, 8, s, bufSz, fragSize);
			seeds.push_back(expired);

			return seeds;
		}

		// fuzz_reorder_gate.cpp's op stream: 3 config bytes, then ops.
		std::vector<Bytes> ReorderGateSeeds()
		{
			std::vector<Bytes> seeds;
			// In order, window 8, 20 ms delay: push +1 six times, 1 ms apart.
			seeds.push_back(Bytes{ 7, 20, 64, 0x04, 1, 0x04, 1, 0x04, 1, 0x04, 1, 0x04, 1, 0x04, 1 });
			// Reordered and duplicated: +2, -1, +2, 0 (dup), flush after 30 ms.
			seeds.push_back(Bytes{ 3, 10, 64, 0x04, 2, 0x04, 0xff, 0x04, 2, 0x04, 0, 0x7a, 0x02 });
			// Epoch restart: epoch 5, pushes, epoch 9, pushes, stale epoch 4.
			seeds.push_back(Bytes{ 15, 50, 16, 0x03, 5, 0x04, 1, 0x04, 1, 0x03, 9, 0x04, 1, 0x03, 4 });
			// Legacy backjump restart: absolute seq 1000, then 3.
			seeds.push_back(Bytes{ 15, 50, 16, 0x01, 0xe8, 0x03, 0, 0, 0, 0, 0, 0, 0x04, 1, 0x01, 3, 0, 0, 0, 0, 0, 0, 0 });
			return seeds;
		}

		// fuzz_tcp_stream.cpp: a max-payload selector byte, then one
		// [u16 length][bytes] record per socket read. Frames carry the
		// canonical keyframe so the Parse() stage sees real data.
		Bytes TcpFrame(const std::vector<char>& payload)
		{
			Bytes out(kTcpFrameHeaderSize + payload.size());
			writeTcpFrameHeader(out.data(), (uint32_t)payload.size());
			std::memcpy(out.data() + kTcpFrameHeaderSize, payload.data(), payload.size());
			return out;
		}

		std::vector<Bytes> TcpStreamSeeds()
		{
			const Bytes frame = TcpFrame(CanonicalKeyframe());
			const uint8_t maxSel = 0xff; // 16336 bytes, enough for the keyframe
			std::vector<Bytes> seeds;

			// Three frames in one read.
			Bytes three;
			for (int i = 0; i < 3; ++i) three.insert(three.end(), frame.begin(), frame.end());
			Bytes oneRead{ maxSel };
			AppendRecord(oneRead, three.data(), three.size());
			seeds.push_back(oneRead);

			// The same bytes in 7-byte reads, so headers split across reads.
			Bytes splitReads{ maxSel };
			for (size_t pos = 0; pos < three.size(); pos += 7)
				AppendRecord(splitReads, three.data() + pos, std::min<size_t>(7, three.size() - pos));
			seeds.push_back(splitReads);

			// Garbage (including a partial magic), then a frame.
			Bytes garbage;
			for (int i = 0; i < 300; ++i) garbage.push_back((uint8_t)(i * 37));
			garbage.insert(garbage.end(), kTcpFrameMagic, kTcpFrameMagic + 6);
			garbage.insert(garbage.end(), frame.begin(), frame.end());
			Bytes garbageSeed{ maxSel };
			AppendRecord(garbageSeed, garbage.data(), garbage.size());
			seeds.push_back(garbageSeed);

			// An oversize header (max payload 16 + 4 * 64 = 272), then a small frame.
			Bytes oversize(kTcpFrameHeaderSize);
			writeTcpFrameHeader(oversize.data(), 100000);
			const std::vector<char> small(200, 'x');
			const Bytes smallFrame = TcpFrame(small);
			oversize.insert(oversize.end(), smallFrame.begin(), smallFrame.end());
			Bytes oversizeSeed{ 4 };
			AppendRecord(oversizeSeed, oversize.data(), oversize.size());
			seeds.push_back(oversizeSeed);

			return seeds;
		}
	}

	const std::vector<std::string>& TargetNames()
	{
		static const std::vector<std::string> names = {
			"parse", "parse_update", "residual", "peek_meta", "udp_reassembly", "reorder_gate", "tcp_stream"
		};
		return names;
	}

	std::vector<Bytes> SeedsFor(const std::string& target)
	{
		if (target == "parse") return ParseSeeds();
		if (target == "parse_update") return ParseUpdateSeeds();
		if (target == "residual") return ResidualSeeds();
		if (target == "peek_meta") return PeekMetaSeeds();
		if (target == "udp_reassembly") return UdpSeeds();
		if (target == "reorder_gate") return ReorderGateSeeds();
		if (target == "tcp_stream") return TcpStreamSeeds();
		return {};
	}

	const std::vector<char>& CanonicalKeyframe()
	{
		static const std::vector<char> keyframe = [] {
			SubjectList list;
			BuildCanonical(list);
			return SerializeKeyframe(list, 1, 7);
		}();
		return keyframe;
	}

	std::vector<char> WrapPayload(const uint8_t* data, size_t size)
	{
		std::vector<char> out(8 + size);
		const uint32_t flags = 1;
		const uint32_t crc = CRCPP::CRC::Calculate(data, size, CRCPP::CRC::CRC_32());
		std::memcpy(out.data(), &flags, 4);
		std::memcpy(out.data() + 4, &crc, 4);
		if (size > 0)
			std::memcpy(out.data() + 8, data, size);
		return out;
	}

	std::vector<Record> SplitRecords(const uint8_t* data, size_t size, size_t maxRecords)
	{
		std::vector<Record> out;
		size_t pos = 0;
		while (pos < size && out.size() < maxRecords)
		{
			size_t len = data[pos];
			if (pos + 1 < size)
				len |= (size_t)data[pos + 1] << 8;
			pos = std::min(size, pos + 2);
			len = std::min(len, size - pos);
			out.push_back(Record{ data + pos, len });
			pos += len;
		}
		return out;
	}

	void AppendRecord(Bytes& out, const uint8_t* data, size_t size)
	{
		const size_t len = std::min(size, (size_t)0xffff);
		out.push_back((uint8_t)(len & 0xff));
		out.push_back((uint8_t)(len >> 8));
		out.insert(out.end(), data, data + len);
	}
}
