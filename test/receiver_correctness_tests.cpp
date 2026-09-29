// WP-S4 receiver correctness: core-level tests for the pieces the UE
// receiver (O3DReceiverSource) builds on.
//
//  - RCV-4:  SkeletonFingerprint covers bone names, and Parse() reports a
//            full descriptor, so renamed bones are republished.
//  - RCV-5:  Parse() reports only the subjects a packet touched, and
//            ReceiverStreamTable keeps one parse and ordering state per
//            sender, so two senders on one channel don't interfere.
//  - RCV-10: ConcealmentEngine::TryRenderAhead() fires once per real frame.
//  - RCV-14: a nameless transform in the middle of a subject keeps later
//            parent ids aligned.
//  - RCV-34: LegacyOrdering resets never touch the gated-path state.
//
// Recorded frames go through the B1 capture writer and ReplayCapture, the
// way a captured session would be fed back into a receiver.
#include "test_framework.h"

#include "o3ds/capture.h"
#include "o3ds/model.h"
#include "o3ds/receiver_streams.h"
#include "o3ds/replay.h"
#include "o3ds/predict/concealment.h"
#include "o3ds/predict/linear_predictor.h"
#include "o3ds_generated.h"

#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace O3DS;

namespace
{
	Transform* AddBone(Subject* subject, const std::string& name, int parent)
	{
		Transform* t = subject->addTransform(name, parent);
		t->transformOrder.push_back(O3DS::TTranslation);
		t->transformOrder.push_back(O3DS::TRotation);
		return t;
	}

	// Builds a three-bone chain under `subjectName` with the given names.
	void BuildChain(SubjectList& list, const std::string& subjectName, const std::vector<std::string>& boneNames)
	{
		Subject* subject = list.addSubject(subjectName);
		for (size_t i = 0; i < boneNames.size(); ++i)
			AddBone(subject, boneNames[i], static_cast<int>(i) - 1);
	}

	std::vector<char> SerializeFull(SubjectList& list, double t, uint64_t seq = 0, uint32_t epoch = 0)
	{
		std::vector<char> buf;
		O3DS_CHECK(list.Serialize(buf, t, seq, seq ? 1000 + seq : 0, epoch) > 0);
		return buf;
	}

	std::vector<char> SerializeDelta(SubjectList& list, double t, uint64_t seq = 0, uint32_t epoch = 0)
	{
		std::vector<char> buf;
		size_t count = 0;
		O3DS_CHECK(list.SerializeUpdate(buf, count, t, seq, seq ? 1000 + seq : 0, epoch) > 0);
		return buf;
	}

	// Writes `packets` to an in-memory .o3dscap and replays it, returning
	// the frames in recorded order.
	std::vector<Frame> RecordAndReplay(const std::vector<std::vector<char>>& packets)
	{
		std::stringstream capture(std::ios::in | std::ios::out | std::ios::binary);
		CaptureHeaderInfo header;
		header.source_desc = "receiver_correctness_tests";
		O3DS_CHECK(WriteCaptureHeader(capture, header));
		uint64_t recvUs = 1000000;
		for (const std::vector<char>& packet : packets)
		{
			CaptureRecord record;
			record.recv_wallclock_us = recvUs;
			record.wire_bytes = packet;
			O3DS_CHECK(WriteCaptureRecord(capture, record));
			recvUs += 16667;
		}

		capture.seekg(0);
		std::vector<Frame> frames;
		ReplayConfig config;
		O3DS_CHECK(ReplayCapture(capture, config, [&frames](Frame&& frame, double) {
			frames.push_back(std::move(frame));
			return true;
		}));
		O3DS_CHECK_EQ(frames.size(), packets.size());
		return frames;
	}

	// A stand-in for what the UE receiver publishes to LiveLink for one
	// subject: bone names and parents, rebuilt only when the skeleton cache
	// is invalid. This is the decision O3DReceiverSource::
	// ProcessParsedSubject makes: rebuild on a full descriptor or a
	// fingerprint change, otherwise reuse the cached names.
	struct PublishedSkeleton
	{
		uint64_t fingerprint = 0;
		bool valid = false;
		std::vector<std::string> names;
		std::vector<int> parents;
		int rebuilds = 0;
	};

	void Publish(std::map<std::string, PublishedSkeleton>& published, Subject& subject, bool fullDescriptor)
	{
		PublishedSkeleton& p = published[subject.mName];
		const uint64_t fingerprint = SkeletonFingerprint(subject);
		if (p.valid && !fullDescriptor && p.fingerprint == fingerprint)
			return;

		p.names.clear();
		p.parents.clear();
		for (Transform* t : subject.mTransforms)
		{
			p.names.push_back(t->mName);
			p.parents.push_back(t->mParentId);
		}
		p.fingerprint = fingerprint;
		p.valid = true;
		++p.rebuilds;
	}

	// One receiver built from the core pieces, mirroring the UE glue:
	// peek, route to the sender's stream, gate or legacy-order, then parse
	// and publish only the touched subjects.
	struct CoreReceiver
	{
		ReceiverStreamTable streams;
		std::map<std::string, PublishedSkeleton> published;
		std::vector<std::string> pushedSubjects; // one entry per published frame
		size_t legacyDropped = 0;
		size_t parseFailures = 0;
		double nowS = 0.0;

		void Emit(uint64_t key, Frame&& frame)
		{
			ReceiverStream* stream = streams.Find(key);
			O3DS_CHECK(stream != nullptr);
			std::vector<ParsedSubjectInfo> touched;
			if (!stream->subjects.Parse(frame.bytes.data(), frame.bytes.size(), nullptr, true, &touched))
			{
				++parseFailures;
				return;
			}
			for (const ParsedSubjectInfo& info : touched)
			{
				Subject* subject = stream->subjects.findSubject(info.name);
				O3DS_CHECK(subject != nullptr);
				Publish(published, *subject, info.fullDescriptor);
				pushedSubjects.push_back(info.name);
			}
		}

		void Receive(Frame&& frame)
		{
			nowS += 0.001;
			PacketMeta meta;
			if (!PeekPacketMeta(frame.bytes.data(), frame.bytes.size(), meta))
			{
				++parseFailures;
				return;
			}
			ReceiverStream& stream = streams.Acquire(meta.stream_key, nowS);
			const uint64_t key = meta.stream_key;
			if (meta.tx_seq == 0)
			{
				if (stream.legacy.Check(meta.time, nowS, LegacyOrderingConfig()) != LegacyOrdering::Decision::Apply)
				{
					++legacyDropped;
					return;
				}
				Emit(key, std::move(frame));
				return;
			}
			stream.gate.Push(std::move(frame), nowS, [this, key](Frame&& f) { Emit(key, std::move(f)); });
		}
	};
}

// ---------------------------------------------------------------------------
// Parse(): touched subjects (RCV-5)
// ---------------------------------------------------------------------------

O3DS_TEST(ReceiverParse_ReportsFullDescriptorsForEverySubjectInPacket)
{
	SubjectList sender;
	BuildChain(sender, "A", { "Hips", "Spine", "Head" });
	BuildChain(sender, "B", { "Hips", "Spine", "Head" });

	SubjectList receiver;
	std::vector<ParsedSubjectInfo> touched;
	const std::vector<char> buf = SerializeFull(sender, 1.0);
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size(), nullptr, true, &touched));

	O3DS_CHECK_EQ(touched.size(), (size_t)2);
	O3DS_CHECK_EQ(touched[0].name, std::string("A"));
	O3DS_CHECK(touched[0].fullDescriptor);
	O3DS_CHECK_EQ(touched[1].name, std::string("B"));
	O3DS_CHECK(touched[1].fullDescriptor);
}

O3DS_TEST(ReceiverParse_UpdateOnlyPacketReportsOnlyItsSubjects)
{
	// The receiver knows A and B; a later packet updates only A. Before
	// WP-S4 the receiver re-pushed B with its old pose on every such packet.
	SubjectList both;
	BuildChain(both, "A", { "Hips", "Spine", "Head" });
	BuildChain(both, "B", { "Hips", "Spine", "Head" });
	SubjectList receiver;
	const std::vector<char> full = SerializeFull(both, 1.0);
	O3DS_CHECK(receiver.Parse(full.data(), full.size()));

	SubjectList onlyA;
	BuildChain(onlyA, "A", { "Hips", "Spine", "Head" });
	onlyA.findSubject("A")->mTransforms[1]->translation.value = Vector3d(0.0, 5.0, 0.0);

	std::vector<ParsedSubjectInfo> touched;
	const std::vector<char> delta = SerializeDelta(onlyA, 1.1);
	O3DS_CHECK(receiver.Parse(delta.data(), delta.size(), nullptr, true, &touched));

	O3DS_CHECK_EQ(touched.size(), (size_t)1);
	O3DS_CHECK_EQ(touched[0].name, std::string("A"));
	O3DS_CHECK(!touched[0].fullDescriptor);
	O3DS_CHECK_EQ(receiver.size(), (size_t)2); // B is still known, just not touched
	O3DS_CHECK(std::abs(receiver.findSubject("A")->mTransforms[1]->translation.value.v[1] - 5.0) < 1e-6);
}

O3DS_TEST(ReceiverParse_UpdateForUnknownSubjectIsNotReported)
{
	SubjectList sender;
	BuildChain(sender, "Ghost", { "Hips", "Spine", "Head" });

	SubjectList receiver;
	std::vector<ParsedSubjectInfo> touched = { ParsedSubjectInfo{ "stale", true } };
	const std::vector<char> delta = SerializeDelta(sender, 1.0);
	O3DS_CHECK(receiver.Parse(delta.data(), delta.size(), nullptr, true, &touched));
	O3DS_CHECK(touched.empty()); // cleared, and nothing applied
}

// ---------------------------------------------------------------------------
// Skeleton fingerprint and renamed bones (RCV-4)
// ---------------------------------------------------------------------------

O3DS_TEST(SkeletonFingerprint_ChangesWithNamesAndParents)
{
	SubjectList a, b, c, d;
	BuildChain(a, "S", { "Hips", "Spine", "Head" });
	BuildChain(b, "S", { "Hips", "Spine", "Head" });
	BuildChain(c, "S", { "Root", "Chest", "Neck" });
	BuildChain(d, "S", { "Hips", "Spine", "Head" });
	d.findSubject("S")->mTransforms[2]->mParentId = 0;

	const uint64_t fa = SkeletonFingerprint(*a.findSubject("S"));
	O3DS_CHECK_EQ(fa, SkeletonFingerprint(*b.findSubject("S")));
	O3DS_CHECK(fa != SkeletonFingerprint(*c.findSubject("S"))); // same topology, new names
	O3DS_CHECK(fa != SkeletonFingerprint(*d.findSubject("S"))); // same names, new parent

	// Length-prefixed names: moving a character across a boundary changes it.
	SubjectList e, f;
	BuildChain(e, "S", { "ab", "c" });
	BuildChain(f, "S", { "a", "bc" });
	O3DS_CHECK(SkeletonFingerprint(*e.findSubject("S")) != SkeletonFingerprint(*f.findSubject("S")));
}

O3DS_TEST(ReceiverReplay_RenamedBonesAreRepublished)
{
	// A sender streams a rig, then swaps to a rig with the same hierarchy
	// but different bone names, and keeps streaming deltas.
	SubjectList rigA;
	BuildChain(rigA, "Actor", { "Hips", "Spine", "Head" });
	SubjectList rigB;
	BuildChain(rigB, "Actor", { "Root", "Chest", "Neck" });

	std::vector<std::vector<char>> packets;
	packets.push_back(SerializeFull(rigA, 1.00));
	rigA.findSubject("Actor")->mTransforms[1]->translation.value = Vector3d(0.0, 1.0, 0.0);
	packets.push_back(SerializeDelta(rigA, 1.02));
	packets.push_back(SerializeFull(rigB, 1.04));
	rigB.findSubject("Actor")->mTransforms[1]->translation.value = Vector3d(0.0, 2.0, 0.0);
	packets.push_back(SerializeDelta(rigB, 1.06));

	CoreReceiver receiver;
	for (Frame& frame : RecordAndReplay(packets))
		receiver.Receive(std::move(frame));

	O3DS_CHECK_EQ(receiver.parseFailures, (size_t)0);
	O3DS_CHECK_EQ(receiver.pushedSubjects.size(), (size_t)4);
	const PublishedSkeleton& p = receiver.published["Actor"];
	O3DS_CHECK_EQ(p.rebuilds, 2); // first sight and the rename; deltas reuse the cache
	O3DS_CHECK_EQ(p.names.size(), (size_t)3);
	O3DS_CHECK_EQ(p.names[0], std::string("Root"));
	O3DS_CHECK_EQ(p.names[1], std::string("Chest"));
	O3DS_CHECK_EQ(p.names[2], std::string("Neck"));
	O3DS_CHECK_EQ(p.parents[2], 1);
}

O3DS_TEST(ReceiverReplay_FullDescriptorInvalidatesCacheEvenWithSameFingerprint)
{
	// A full descriptor always rebuilds, so a legacy stream (full Subject
	// every frame) with identical names publishes the same names again
	// rather than anything stale.
	SubjectList rig;
	BuildChain(rig, "Actor", { "Hips", "Spine", "Head" });
	std::vector<std::vector<char>> packets = { SerializeFull(rig, 1.0), SerializeFull(rig, 1.1) };

	CoreReceiver receiver;
	for (Frame& frame : RecordAndReplay(packets))
		receiver.Receive(std::move(frame));
	O3DS_CHECK_EQ(receiver.published["Actor"].rebuilds, 2);
	O3DS_CHECK_EQ(receiver.published["Actor"].names[0], std::string("Hips"));
}

// ---------------------------------------------------------------------------
// Null transform in the middle keeps parents aligned (RCV-14)
// ---------------------------------------------------------------------------

namespace
{
	// root, <nameless>(->root), a(->root), b(->root), c(->a). Before the
	// fix the nameless node was dropped, so c's parent id 2 pointed at b.
	std::vector<char> BuildSubjectWithNamelessNode()
	{
		flatbuffers::FlatBufferBuilder fbb;
		const O3DS::Data::Translation tr(1.0f, 2.0f, 3.0f);
		std::vector<int8_t> comps = { O3DS::Data::Component_Translation };

		struct Node { const char* name; int parent; };
		const Node nodes[] = { { "root", -1 }, { nullptr, 0 }, { "a", 0 }, { "b", 0 }, { "c", 2 } };

		std::vector<flatbuffers::Offset<O3DS::Data::Transform>> out;
		for (const Node& n : nodes)
		{
			auto name = n.name ? fbb.CreateString(n.name) : flatbuffers::Offset<flatbuffers::String>();
			auto components = fbb.CreateVector(comps);
			out.push_back(O3DS::Data::CreateTransform(fbb, n.parent, name, &tr, nullptr, nullptr, 0, components));
		}
		auto subject = O3DS::Data::CreateSubject(fbb, fbb.CreateVector(out), fbb.CreateString("Actor"));
		std::vector<flatbuffers::Offset<O3DS::Data::Subject>> subjects = { subject };
		fbb.Finish(O3DS::Data::CreateSubjectList(fbb, fbb.CreateVector(subjects), 0, 1.0));

		std::vector<char> buf;
		finalize(fbb, buf, 1);
		return buf;
	}
}

O3DS_TEST(ReceiverParse_NamelessTransformInMiddleKeepsParentsCorrect)
{
	const std::vector<char> buf = BuildSubjectWithNamelessNode();

	SubjectList receiver;
	O3DS_CHECK(receiver.Parse(buf.data(), buf.size()));
	Subject* s = receiver.findSubject("Actor");
	O3DS_CHECK(s != nullptr);
	O3DS_CHECK_EQ(s->size(), (size_t)5);

	O3DS_CHECK_EQ(s->mTransforms[1]->mName, std::string(kUnnamedTransformPrefix) + "1");
	O3DS_CHECK_EQ(s->mTransforms[1]->mParentId, 0);

	Transform* c = s->mTransforms[4];
	O3DS_CHECK_EQ(c->mName, std::string("c"));
	O3DS_CHECK_EQ(s->mTransforms[(size_t)c->mParentId]->mName, std::string("a"));
	O3DS_CHECK_EQ(s->mTransforms[3]->mName, std::string("b"));
	O3DS_CHECK_EQ(s->mTransforms[(size_t)s->mTransforms[3]->mParentId]->mName, std::string("root"));
}

O3DS_TEST(ReceiverReplay_NamelessTransformPublishesAlignedParents)
{
	CoreReceiver receiver;
	for (Frame& frame : RecordAndReplay({ BuildSubjectWithNamelessNode() }))
		receiver.Receive(std::move(frame));

	O3DS_CHECK_EQ(receiver.parseFailures, (size_t)0);
	const PublishedSkeleton& p = receiver.published["Actor"];
	O3DS_CHECK_EQ(p.names.size(), (size_t)5);
	O3DS_CHECK_EQ(p.names.size(), p.parents.size());
	O3DS_CHECK_EQ(p.names[(size_t)p.parents[4]], std::string("a"));
}

// ---------------------------------------------------------------------------
// Two senders on one channel (RCV-5)
// ---------------------------------------------------------------------------

O3DS_TEST(StreamKey_IsOrderIndependentAndZeroWithoutNames)
{
	O3DS_CHECK_EQ(StreamKeyForNames({}), (uint64_t)0);
	O3DS_CHECK_EQ(StreamKeyForNames({ "A", "B" }), StreamKeyForNames({ "B", "A", "A" }));
	O3DS_CHECK(StreamKeyForNames({ "A" }) != StreamKeyForNames({ "B" }));
	O3DS_CHECK(StreamKeyForNames({ "A" }) != 0);
}

O3DS_TEST(ReceiverReplay_TwoGatedSendersOnOneChannelDontInterfere)
{
	// Two senders with their own tx_seq spaces and epochs, interleaved on one
	// channel, each sending a full sync then deltas.
	SubjectList senderA;
	BuildChain(senderA, "Alice", { "Hips", "Spine", "Head" });
	SubjectList senderB;
	BuildChain(senderB, "Bob", { "Hips", "Spine", "Head" });

	const int kFrames = 20;
	std::vector<std::vector<char>> packets;
	for (int i = 0; i < kFrames; ++i)
	{
		const uint64_t seq = static_cast<uint64_t>(i + 1);
		senderA.findSubject("Alice")->mTransforms[1]->translation.value = Vector3d(0.0, 1.0 + i, 0.0);
		senderB.findSubject("Bob")->mTransforms[1]->translation.value = Vector3d(0.0, 100.0 + i, 0.0);
		packets.push_back(i == 0 ? SerializeFull(senderA, 10.0 + i, seq, 100) : SerializeDelta(senderA, 10.0 + i, seq, 100));
		packets.push_back(i == 0 ? SerializeFull(senderB, 500.0 + i, seq + 1000, 200) : SerializeDelta(senderB, 500.0 + i, seq + 1000, 200));
	}

	CoreReceiver receiver;
	for (Frame& frame : RecordAndReplay(packets))
		receiver.Receive(std::move(frame));

	O3DS_CHECK_EQ(receiver.parseFailures, (size_t)0);
	O3DS_CHECK_EQ(receiver.streams.Size(), (size_t)2);

	// Every packet was published exactly once, for its own subject only.
	O3DS_CHECK_EQ(receiver.pushedSubjects.size(), packets.size());
	for (size_t i = 0; i < receiver.pushedSubjects.size(); ++i)
		O3DS_CHECK_EQ(receiver.pushedSubjects[i], std::string(i % 2 == 0 ? "Alice" : "Bob"));

	receiver.streams.ForEach([&](uint64_t, ReceiverStream& stream) {
		O3DS_CHECK_EQ(stream.gate.Stats().stale_dropped, (uint64_t)0);
		O3DS_CHECK_EQ(stream.gate.Stats().dup_dropped, (uint64_t)0);
		O3DS_CHECK_EQ(stream.gate.Stats().delivered, (uint64_t)kFrames);
		O3DS_CHECK_EQ(stream.subjects.size(), (size_t)1);
	});

	// Final poses are each sender's own last values.
	const uint64_t keyA = StreamKeyForNames({ "Alice" });
	const uint64_t keyB = StreamKeyForNames({ "Bob" });
	O3DS_CHECK(receiver.streams.Find(keyA) != nullptr);
	O3DS_CHECK(receiver.streams.Find(keyB) != nullptr);
	Subject* alice = receiver.streams.Find(keyA)->subjects.findSubject("Alice");
	Subject* bob = receiver.streams.Find(keyB)->subjects.findSubject("Bob");
	O3DS_CHECK(alice != nullptr && bob != nullptr);
	O3DS_CHECK(std::abs(alice->mTransforms[1]->translation.value.v[1] - (1.0 + kFrames - 1)) < 1e-4);
	O3DS_CHECK(std::abs(bob->mTransforms[1]->translation.value.v[1] - (100.0 + kFrames - 1)) < 1e-4);
}

O3DS_TEST(ReceiverReplay_SharedGateWouldDropTheOlderSender)
{
	// Documents why the gate is per stream: one gate fed both senders
	// treats the older epoch's frames as stragglers from a previous session.
	SubjectList senderA;
	BuildChain(senderA, "Alice", { "Hips" });
	SubjectList senderB;
	BuildChain(senderB, "Bob", { "Hips" });

	ReorderGate shared;
	size_t delivered = 0;
	double now = 0.0;
	for (int i = 0; i < 5; ++i)
	{
		const uint64_t seq = static_cast<uint64_t>(i + 1);
		for (int s = 0; s < 2; ++s)
		{
			std::vector<char> bytes = (s == 0) ? SerializeFull(senderA, i, seq, 100) : SerializeFull(senderB, i, seq, 200);
			Frame f;
			PacketMeta meta;
			O3DS_CHECK(PeekPacketMeta(bytes.data(), bytes.size(), meta));
			f.seq = meta.tx_seq;
			f.epoch = meta.frame_epoch;
			f.bytes = std::move(bytes);
			now += 0.001;
			shared.Push(std::move(f), now, [&delivered](Frame&&) { ++delivered; });
		}
	}
	O3DS_CHECK(shared.Stats().stale_dropped > 0);
	O3DS_CHECK(delivered < 10);
}

O3DS_TEST(ReceiverReplay_TwoLegacySendersWithDifferentClocksDontInterfere)
{
	// No tx_seq: ordering falls back to SubjectList.time. The two senders'
	// content clocks are far apart; one shared "last applied time" would
	// drop every frame of the sender with the smaller clock.
	SubjectList senderA;
	BuildChain(senderA, "Alice", { "Hips", "Spine" });
	SubjectList senderB;
	BuildChain(senderB, "Bob", { "Hips", "Spine" });

	std::vector<std::vector<char>> packets;
	for (int i = 0; i < 10; ++i)
	{
		packets.push_back(SerializeFull(senderA, 5000.0 + i * 0.016));
		packets.push_back(SerializeFull(senderB, 3.0 + i * 0.016));
	}

	CoreReceiver receiver;
	for (Frame& frame : RecordAndReplay(packets))
		receiver.Receive(std::move(frame));

	O3DS_CHECK_EQ(receiver.legacyDropped, (size_t)0);
	O3DS_CHECK_EQ(receiver.pushedSubjects.size(), packets.size());
	O3DS_CHECK_EQ(receiver.streams.Size(), (size_t)2);
}

O3DS_TEST(ReceiverReplay_FullDescriptorFromOneSenderKeepsTheOtherSendersSubjects)
{
	// Before WP-S4 one shared SubjectList was parsed with clearInactive, so
	// sender A's full sync deleted sender B's subject and B's deltas were
	// then ignored until B's next full sync.
	SubjectList senderA;
	BuildChain(senderA, "Alice", { "Hips", "Spine" });
	SubjectList senderB;
	BuildChain(senderB, "Bob", { "Hips", "Spine" });

	std::vector<std::vector<char>> packets;
	packets.push_back(SerializeFull(senderB, 1.0));
	packets.push_back(SerializeFull(senderA, 1.0));
	senderB.findSubject("Bob")->mTransforms[1]->translation.value = Vector3d(7.0, 0.0, 0.0);
	packets.push_back(SerializeDelta(senderB, 1.1));

	CoreReceiver receiver;
	for (Frame& frame : RecordAndReplay(packets))
		receiver.Receive(std::move(frame));

	O3DS_CHECK_EQ(receiver.pushedSubjects.size(), (size_t)3);
	O3DS_CHECK_EQ(receiver.pushedSubjects[2], std::string("Bob"));
	Subject* bob = receiver.streams.Find(StreamKeyForNames({ "Bob" }))->subjects.findSubject("Bob");
	O3DS_CHECK(bob != nullptr);
	O3DS_CHECK(std::abs(bob->mTransforms[1]->translation.value.v[0] - 7.0) < 1e-6);
}

// ---------------------------------------------------------------------------
// ReceiverStreamTable bounds
// ---------------------------------------------------------------------------

O3DS_TEST(ReceiverStreamTable_EvictsLeastRecentlySeenWhenFull)
{
	ReceiverStreamTable table(2);
	table.Acquire(1, 1.0);
	table.Acquire(2, 2.0);
	table.Acquire(1, 3.0); // 2 is now the oldest
	table.Acquire(3, 4.0);
	O3DS_CHECK_EQ(table.Size(), (size_t)2);
	O3DS_CHECK(table.Find(1) != nullptr);
	O3DS_CHECK(table.Find(2) == nullptr);
	O3DS_CHECK(table.Find(3) != nullptr);
}

O3DS_TEST(ReceiverStreamTable_PruneIdleAndWorldMatrixFlag)
{
	ReceiverStreamTable table(8, false);
	O3DS_CHECK(!table.Acquire(1, 0.0).subjects.mComputeWorldMatrices);
	table.Acquire(2, 4.0);
	O3DS_CHECK_EQ(table.PruneIdle(6.0, 5.0), (size_t)1);
	O3DS_CHECK(table.Find(1) == nullptr);
	O3DS_CHECK(table.Find(2) != nullptr);
	table.Clear();
	O3DS_CHECK_EQ(table.Size(), (size_t)0);
}

O3DS_TEST(PeekPacketMeta_RejectsBadInputAndReadsFields)
{
	PacketMeta meta;
	O3DS_CHECK(!PeekPacketMeta(nullptr, 0, meta));
	const char junk[16] = { 1, 0, 0, 0, 9, 9, 9, 9, 'x', 'y', 'z' };
	O3DS_CHECK(!PeekPacketMeta(junk, sizeof(junk), meta));
	O3DS_CHECK_EQ(meta.stream_key, (uint64_t)0);

	SubjectList sender;
	BuildChain(sender, "Alice", { "Hips" });
	const std::vector<char> buf = SerializeFull(sender, 2.5, 7, 42);
	O3DS_CHECK(PeekPacketMeta(buf.data(), buf.size(), meta));
	O3DS_CHECK_EQ(meta.tx_seq, (uint64_t)7);
	O3DS_CHECK_EQ(meta.frame_epoch, (uint32_t)42);
	O3DS_CHECK(meta.time == 2.5);
	O3DS_CHECK_EQ(meta.stream_key, StreamKeyForNames({ "Alice" }));

	// Non-finite content time is rejected before any ordering decision.
	std::vector<char> nanBuf = SerializeFull(sender, std::nan(""), 8, 42);
	O3DS_CHECK(!PeekPacketMeta(nanBuf.data(), nanBuf.size(), meta));
}

// ---------------------------------------------------------------------------
// LegacyOrdering (RCV-34)
// ---------------------------------------------------------------------------

O3DS_TEST(LegacyOrdering_DropsDuplicatesAndOutOfOrder)
{
	LegacyOrdering ordering;
	LegacyOrderingConfig config;
	O3DS_CHECK(ordering.Check(1.0, 0.0, config) == LegacyOrdering::Decision::Apply);
	O3DS_CHECK(ordering.Check(1.0, 0.01, config) == LegacyOrdering::Decision::Duplicate);
	O3DS_CHECK(ordering.Check(0.9, 0.02, config) == LegacyOrdering::Decision::OutOfOrder);
	O3DS_CHECK(ordering.Check(1.1, 0.03, config) == LegacyOrdering::Decision::Apply);

	config.dropOutOfOrder = false;
	O3DS_CHECK(ordering.Check(1.05, 0.04, config) == LegacyOrdering::Decision::Apply);
}

O3DS_TEST(LegacyOrdering_ResetsOnJumpBackAndSilence)
{
	LegacyOrdering ordering;
	LegacyOrderingConfig config;
	O3DS_CHECK(ordering.Check(100.0, 0.0, config) == LegacyOrdering::Decision::Apply);

	// A sender restart: its clock jumps back by more than the threshold.
	O3DS_CHECK(ordering.Check(2.0, 0.1, config) == LegacyOrdering::Decision::Apply);
	O3DS_CHECK(ordering.LastCheckReset());

	// A small step back is only out of order.
	O3DS_CHECK(ordering.Check(1.5, 0.2, config) == LegacyOrdering::Decision::OutOfOrder);
	O3DS_CHECK(!ordering.LastCheckReset());

	// After silence longer than the threshold, an older time is accepted.
	O3DS_CHECK(ordering.Check(1.8, 3.0, config) == LegacyOrdering::Decision::Apply);
	O3DS_CHECK(ordering.LastCheckReset());
}

O3DS_TEST(LegacyOrdering_ResetDoesNotTouchGatedState)
{
	// RCV-34: a legacy-path reset used to rebuild the ReorderGate and clock
	// estimator too. Now a stream's legacy ordering is separate state.
	ReceiverStreamTable table;
	ReceiverStream& stream = table.Acquire(1, 0.0);

	size_t delivered = 0;
	auto emit = [&delivered](Frame&&) { ++delivered; };
	for (uint64_t seq = 1; seq <= 3; ++seq)
	{
		Frame f;
		f.seq = seq;
		f.epoch = 5;
		stream.gate.Push(std::move(f), 0.001 * seq, emit);
	}
	O3DS_CHECK_EQ(delivered, (size_t)3);

	LegacyOrderingConfig config;
	stream.legacy.Check(100.0, 0.0, config);
	stream.legacy.Check(1.0, 0.1, config); // jump back: legacy reset
	O3DS_CHECK(stream.legacy.LastCheckReset());

	// The gate still remembers seq 3, so a replayed seq 3 is a duplicate.
	Frame dup;
	dup.seq = 3;
	dup.epoch = 5;
	stream.gate.Push(std::move(dup), 0.01, emit);
	O3DS_CHECK_EQ(delivered, (size_t)3);
	O3DS_CHECK_EQ(stream.gate.Stats().dup_dropped, (uint64_t)1);
}

// ---------------------------------------------------------------------------
// Render-ahead once per real frame (RCV-10)
// ---------------------------------------------------------------------------

O3DS_TEST(RenderAhead_FiresOncePerRealFrame)
{
	ConcealmentConfig config;
	config.renderAheadSeconds = 0.05;
	ConcealmentEngine engine(std::make_unique<LinearPredictor>(), config);

	PoseSample real;
	real.t = 0.0;
	real.translations.push_back(Vector3d(0.0, 0.0, 0.0));
	real.rotations.push_back(Quat(0.0, 0.0, 0.0, 1.0));
	real.scales.push_back(Vector3d(1.0, 1.0, 1.0));
	engine.ObserveRealFrame(real);

	PoseSample out;
	O3DS_CHECK(engine.TryRenderAhead(out));
	O3DS_CHECK(!engine.TryRenderAhead(out)); // same target, not produced again
	O3DS_CHECK(!engine.TryRenderAhead(out));

	real.t = 0.016;
	engine.ObserveRealFrame(real);
	O3DS_CHECK(engine.TryRenderAhead(out));
	O3DS_CHECK(std::abs(out.t - (0.016 + 0.05)) < 1e-9);
	O3DS_CHECK(!engine.TryRenderAhead(out));
	O3DS_CHECK_EQ(engine.Metrics().renderAheadFrameCount, (uint64_t)2);

	engine.Reset();
	O3DS_CHECK(!engine.TryRenderAhead(out));
}
