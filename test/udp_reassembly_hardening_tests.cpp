// WP-S2 acceptance tests for src/o3ds/udp_fragment.h reassembly hardening:
// CORE-2 (mixed fragment geometry), CORE-3 / TRB-15 (bounded state, expiry,
// wrapping ids, per-source keys, max message size), CORE-4 (no empty frames,
// rejected fragments don't disturb other messages) and CORE-22 (explicit
// little-endian header encoding).
#include "test_framework.h"

#include "o3ds/udp_fragment.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
	std::vector<char> MakeFrag(uint32_t id, uint32_t seq, uint32_t bufSz, uint32_t fragSize, size_t payloadLen, char fill = 'x')
	{
		std::vector<char> out(kUdpFragmentHeaderSize + payloadLen, fill);
		UdpFragmentHeader header = { id, seq, bufSz, fragSize };
		writeUdpFragmentHeader(header, out.data());
		return out;
	}

	// Sends every fragment of msg (fragSize bytes per fragment) as message id.
	void SendAll(UdpMapper& mapper, uint64_t source, uint32_t id, const std::string& msg, size_t fragSize, uint64_t nowMs)
	{
		UdpFragmenter fragmenter(msg.data(), msg.size(), fragSize);
		std::vector<char> frag;
		for (uint32_t seq = 0; seq < (uint32_t)fragmenter.mFrames; seq++)
		{
			O3DS_CHECK(fragmenter.makeFragment(id, seq, frag));
			O3DS_CHECK(mapper.addFragment(source, frag.data(), frag.size(), nowMs));
		}
	}

	std::string AsString(const std::vector<char>& v)
	{
		return std::string(v.begin(), v.end());
	}
}

O3DS_TEST(UdpHeader_EncodeDecode_RoundTripIsLittleEndian)
{
	UdpFragmentHeader in = { 0x04030201u, 0x08070605u, 0x0c0b0a09u, 0x100f0e0du };
	char buf[kUdpFragmentHeaderSize + 1];
	// Write at an odd offset to exercise unaligned access.
	writeUdpFragmentHeader(in, buf + 1);
	// v2 prefix (ADR 0009 item 5): magic 'O3DF', version 2, flags and reserved 0.
	const unsigned char prefix[8] = { 'O', '3', 'D', 'F', 2, 0, 0, 0 };
	for (int i = 0; i < 8; i++)
		O3DS_CHECK_EQ((int)(unsigned char)buf[1 + i], (int)prefix[i]);
	for (int i = 0; i < 16; i++)
		O3DS_CHECK_EQ((int)(unsigned char)buf[9 + i], i + 1);

	UdpFragmentHeader out = {};
	O3DS_CHECK(readUdpFragmentHeader(buf + 1, kUdpFragmentHeaderSize, out));
	O3DS_CHECK_EQ(out.id, in.id);
	O3DS_CHECK_EQ(out.seq, in.seq);
	O3DS_CHECK_EQ(out.totalSize, in.totalSize);
	O3DS_CHECK_EQ(out.fragSize, in.fragSize);

	O3DS_CHECK(!readUdpFragmentHeader(buf, kUdpFragmentHeaderSize - 1, out));
}

O3DS_TEST(UdpHeader_RejectsAnythingButV2)
{
	UdpFragmentHeader in = { 7u, 0u, 100u, 100u };
	char buf[kUdpFragmentHeaderSize];
	UdpFragmentHeader out = {};

	writeUdpFragmentHeader(in, buf);
	O3DS_CHECK(readUdpFragmentHeader(buf, sizeof(buf), out));

	// Wrong magic, version, flags or reserved byte: not a v2 fragment.
	for (int offset : { 0, 3, 4, 5, 6, 7 })
	{
		writeUdpFragmentHeader(in, buf);
		buf[offset] = (char)(buf[offset] ^ 0x40);
		O3DS_CHECK(!readUdpFragmentHeader(buf, sizeof(buf), out));
	}

	// A legacy 16-byte header (four u32, no magic) is rejected (ADR 0009 Q3).
	const unsigned char legacy[16] = { 7, 0, 0, 0, 0, 0, 0, 0, 100, 0, 0, 0, 100, 0, 0, 0 };
	char legacyPadded[kUdpFragmentHeaderSize] = {};
	memcpy(legacyPadded, legacy, sizeof(legacy));
	O3DS_CHECK(!readUdpFragmentHeader(legacyPadded, sizeof(legacyPadded), out));
}

O3DS_TEST(UdpClassify_FirstFourBytesDecide)
{
	UdpFragmentHeader header = { 1u, 0u, 10u, 10u };
	char frag[kUdpFragmentHeaderSize];
	writeUdpFragmentHeader(header, frag);
	O3DS_CHECK(udpClassifyDatagram(frag, sizeof(frag)) == UdpDatagramKind::Fragment);

	const char envelopeV1[8] = { 'O', '3', 'D', 'A', 1, 1, 0, 0 };
	const char envelopeV2[8] = { 'O', '3', 'D', 'U', 2, 1, 0, 0 };
	O3DS_CHECK(udpClassifyDatagram(envelopeV1, sizeof(envelopeV1)) == UdpDatagramKind::Envelope);
	O3DS_CHECK(udpClassifyDatagram(envelopeV2, sizeof(envelopeV2)) == UdpDatagramKind::Envelope);

	const char frameV1[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };
	const char frameV2[8] = { 2, 0, 0, 0, 0, 0, 0, 0 };
	const char frameV3[8] = { 3, 0, 0, 0, 0, 0, 0, 0 };
	O3DS_CHECK(udpClassifyDatagram(frameV1, sizeof(frameV1)) == UdpDatagramKind::Frame);
	O3DS_CHECK(udpClassifyDatagram(frameV2, sizeof(frameV2)) == UdpDatagramKind::Frame);
	// A newer protocol still reaches the parser, which reports it.
	O3DS_CHECK(udpClassifyDatagram(frameV3, sizeof(frameV3)) == UdpDatagramKind::Frame);

	const char zeroWord[8] = { 0, 0, 0, 0, 1, 0, 0, 0 };
	const char legacyFragment[8] = { 0x2A, 0x01, 0, 0, 0, 0, 0, 0 }; // message id 298
	O3DS_CHECK(udpClassifyDatagram(zeroWord, sizeof(zeroWord)) == UdpDatagramKind::Unknown);
	O3DS_CHECK(udpClassifyDatagram(legacyFragment, sizeof(legacyFragment)) == UdpDatagramKind::Unknown);
	O3DS_CHECK(udpClassifyDatagram(frag, 3) == UdpDatagramKind::Unknown);
	O3DS_CHECK(udpClassifyDatagram(nullptr, 8) == UdpDatagramKind::Unknown);
}

O3DS_TEST(UdpFragmenter_OutOfRangeSeqAndZeroFragSize_AreSafe)
{
	const char* msg = "abcdef";
	UdpFragmenter fragmenter(msg, 6, 4);
	O3DS_CHECK_EQ(fragmenter.mFrames, (size_t)2);
	std::vector<char> out;
	O3DS_CHECK(!fragmenter.makeFragment(1, 2, out));
	O3DS_CHECK(out.empty());

	UdpFragmenter zero(msg, 6, 0);
	O3DS_CHECK_EQ(zero.mFrames, (size_t)0);
	O3DS_CHECK(!zero.makeFragment(1, 0, out));
}

O3DS_TEST(UdpMapper_RoundTrip_ReassemblesExactly)
{
	UdpMapper mapper;
	const std::string msg = "the quick brown fox jumps over the lazy dog";
	SendAll(mapper, 1, 42, msg, 10, 0);

	std::vector<char> out;
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == msg);
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)0);
	O3DS_CHECK_EQ(mapper.bytesInUse(), (size_t)0);
	O3DS_CHECK(!mapper.getFrame(out));
}

O3DS_TEST(UdpMapper_OutOfOrderAndDuplicateFragments_Reassemble)
{
	UdpMapper mapper;
	std::vector<char> f0 = MakeFrag(3, 0, 25, 10, 10, 'a');
	std::vector<char> f1 = MakeFrag(3, 1, 25, 10, 10, 'b');
	std::vector<char> f2 = MakeFrag(3, 2, 25, 10, 5, 'c');
	O3DS_CHECK(mapper.addFragment(1, f2.data(), f2.size(), 0));
	O3DS_CHECK(mapper.addFragment(1, f0.data(), f0.size(), 0));
	O3DS_CHECK(mapper.addFragment(1, f0.data(), f0.size(), 0)); // duplicate
	std::vector<char> out;
	O3DS_CHECK(!mapper.getFrame(out));
	O3DS_CHECK(mapper.addFragment(1, f1.data(), f1.size(), 0));
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == std::string(10, 'a') + std::string(10, 'b') + std::string(5, 'c'));
}

// CORE-2: the `frag` PoC. The first fragment locks bufSz=2000/fragSize=1000
// (2 frames); a later fragment claiming fragSize=1 (2000 frames), seq=1999
// used to write mFound[1999] on a 2-element vector.
O3DS_TEST(UdpCombiner_MixedFragSize_IsRejectedWithoutCrash)
{
	UdpMapper mapper;
	std::vector<char> f1 = MakeFrag(7, 0, 2000, 1000, 1000);
	std::vector<char> f2 = MakeFrag(7, 1999, 2000, 1, 1);
	O3DS_CHECK(mapper.addFragment(1, f1.data(), f1.size(), 0));
	O3DS_CHECK(!mapper.addFragment(1, f2.data(), f2.size(), 0));
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)1);

	// The original message still completes normally.
	std::vector<char> f3 = MakeFrag(7, 1, 2000, 1000, 1000);
	O3DS_CHECK(mapper.addFragment(1, f3.data(), f3.size(), 0));
	std::vector<char> out;
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK_EQ(out.size(), (size_t)2000);
}

O3DS_TEST(UdpCombiner_MixedBufSz_IsRejected)
{
	UdpCombiner combiner;
	std::vector<char> f1 = MakeFrag(7, 0, 2000, 1000, 1000);
	std::vector<char> f2 = MakeFrag(7, 1, 1500, 1000, 500); // consistent on its own
	O3DS_CHECK(combiner.addFragment(f1.data(), f1.size()));
	O3DS_CHECK(!combiner.addFragment(f2.data(), f2.size()));
	O3DS_CHECK(!combiner.isComplete());
	O3DS_CHECK_EQ(combiner.mFound.size(), (size_t)2);
}

O3DS_TEST(UdpCombiner_EmptyCombiner_IsNotComplete)
{
	UdpCombiner combiner;
	O3DS_CHECK(!combiner.isComplete());
}

O3DS_TEST(UdpMapper_OversizedBufSz_IsRejected)
{
	UdpReassemblyConfig config;
	config.maxMessageSize = 64 * 1024;
	UdpMapper mapper(config);

	std::vector<char> tooBig = MakeFrag(1, 0, 64 * 1024 + 1, 1000, 1000);
	O3DS_CHECK(!mapper.addFragment(1, tooBig.data(), tooBig.size(), 0));
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)0);
	O3DS_CHECK_EQ(mapper.bytesInUse(), (size_t)0);

	// The default cap (4 MiB) replaces the previous hard-coded 64 MB.
	UdpMapper defaults;
	std::vector<char> overDefault = MakeFrag(1, 0, (uint32_t)kUdpDefaultMaxMessageSize + 1, 1000, 1000);
	O3DS_CHECK(!defaults.addFragment(1, overDefault.data(), overDefault.size(), 0));
	std::vector<char> atDefault = MakeFrag(1, 0, (uint32_t)kUdpDefaultMaxMessageSize, 1000, 1000);
	O3DS_CHECK(defaults.addFragment(1, atDefault.data(), atDefault.size(), 0));

	std::vector<char> maxU32 = MakeFrag(2, 0, 0xFFFFFFFFu, 1000, 1000);
	O3DS_CHECK(!defaults.addFragment(1, maxU32.data(), maxU32.size(), 0));
}

// CORE-3 / TRB-15: the `dos` PoC scaled up. 10,000 distinct spoofed first
// fragments, each claiming a maximum-size message, must keep the reserved
// reassembly memory under the configured caps.
O3DS_TEST(UdpMapper_TenThousandSpoofedFirstFragments_StayUnderCap)
{
	UdpReassemblyConfig config; // defaults: 8 messages, 4 MiB each, 16 MiB total
	UdpMapper mapper(config);

	size_t peakBytes = 0;
	size_t peakInFlight = 0;
	for (uint32_t id = 0; id < 10000; id++)
	{
		std::vector<char> f = MakeFrag(id, 0, (uint32_t)config.maxMessageSize, 1000, 1000);
		O3DS_CHECK(mapper.addFragment(id % 97, f.data(), f.size(), 0));
		if (mapper.bytesInUse() > peakBytes) peakBytes = mapper.bytesInUse();
		if (mapper.inFlightMessages() > peakInFlight) peakInFlight = mapper.inFlightMessages();
	}

	O3DS_CHECK(peakBytes <= config.maxTotalBytes);
	O3DS_CHECK(peakInFlight <= config.maxInFlightMessages);
	O3DS_CHECK(mapper.stats().messagesEvicted >= 10000 - config.maxInFlightMessages);

	std::vector<char> out;
	O3DS_CHECK(!mapper.getFrame(out));

	// A legitimate message still gets through after the flood.
	SendAll(mapper, 5000, 1, "hello after the flood", 8, 0);
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == "hello after the flood");
}

O3DS_TEST(UdpMapper_InFlightCountCap_EvictsOldest)
{
	UdpReassemblyConfig config;
	config.maxInFlightMessages = 2;
	UdpMapper mapper(config);

	std::vector<char> a0 = MakeFrag(1, 0, 20, 10, 10, 'a');
	std::vector<char> b0 = MakeFrag(2, 0, 20, 10, 10, 'b');
	std::vector<char> c0 = MakeFrag(3, 0, 20, 10, 10, 'c');
	O3DS_CHECK(mapper.addFragment(1, a0.data(), a0.size(), 0));
	O3DS_CHECK(mapper.addFragment(2, b0.data(), b0.size(), 0));
	O3DS_CHECK(mapper.addFragment(3, c0.data(), c0.size(), 0)); // evicts (1, 1)
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)2);
	O3DS_CHECK_EQ(mapper.stats().messagesEvicted, (uint64_t)1);

	// Message 1's second half now starts a fresh (incomplete) entry.
	std::vector<char> a1 = MakeFrag(1, 1, 20, 10, 10, 'a');
	O3DS_CHECK(mapper.addFragment(1, a1.data(), a1.size(), 0)); // evicts (2, 2)
	std::vector<char> out;
	O3DS_CHECK(!mapper.getFrame(out));

	std::vector<char> c1 = MakeFrag(3, 1, 20, 10, 10, 'c');
	O3DS_CHECK(mapper.addFragment(3, c1.data(), c1.size(), 0));
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == std::string(20, 'c'));
}

O3DS_TEST(UdpMapper_TotalBytesCap_EvictsOldest)
{
	UdpReassemblyConfig config;
	config.maxMessageSize = 1000;
	config.maxTotalBytes = 2500;
	config.maxInFlightMessages = 100;
	UdpMapper mapper(config);

	for (uint32_t id = 1; id <= 5; id++)
	{
		std::vector<char> f = MakeFrag(id, 0, 1000, 100, 100);
		O3DS_CHECK(mapper.addFragment(1, f.data(), f.size(), 0));
		O3DS_CHECK(mapper.bytesInUse() <= config.maxTotalBytes);
	}
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)2);
	O3DS_CHECK_EQ(mapper.bytesInUse(), (size_t)2000);
}

O3DS_TEST(UdpMapper_IncompleteMessage_ExpiresAfterTimeout)
{
	UdpReassemblyConfig config;
	config.messageTimeoutMs = 100;
	UdpMapper mapper(config);

	std::vector<char> f0 = MakeFrag(9, 0, 20, 10, 10);
	std::vector<char> f1 = MakeFrag(9, 1, 20, 10, 10);
	O3DS_CHECK(mapper.addFragment(1, f0.data(), f0.size(), 1000));

	mapper.expire(1099);
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)1);
	mapper.expire(1100);
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)0);
	O3DS_CHECK_EQ(mapper.bytesInUse(), (size_t)0);
	O3DS_CHECK_EQ(mapper.stats().messagesExpired, (uint64_t)1);

	// The late second half starts a new entry and never completes alone.
	O3DS_CHECK(mapper.addFragment(1, f1.data(), f1.size(), 1150));
	std::vector<char> out;
	O3DS_CHECK(!mapper.getFrame(out));

	// Expiry is also applied implicitly by addFragment.
	std::vector<char> other = MakeFrag(10, 0, 20, 10, 10);
	O3DS_CHECK(mapper.addFragment(1, other.data(), other.size(), 1300));
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)1);

	// A message that completes within the timeout is delivered.
	std::vector<char> other1 = MakeFrag(10, 1, 20, 10, 10);
	O3DS_CHECK(mapper.addFragment(1, other1.data(), other1.size(), 1399));
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK_EQ(out.size(), (size_t)20);
}

O3DS_TEST(UdpMapper_MessageIdSerialComparison_Wraps)
{
	O3DS_CHECK(udpMessageIdLess(1, 2));
	O3DS_CHECK(!udpMessageIdLess(2, 1));
	O3DS_CHECK(!udpMessageIdLess(5, 5));
	O3DS_CHECK(udpMessageIdLess(0xFFFFFFFFu, 0));
	O3DS_CHECK(udpMessageIdLess(0xFFFFFFF0u, 2));
	O3DS_CHECK(!udpMessageIdLess(2, 0xFFFFFFF0u));

	UdpMapper mapper;
	std::vector<char> out;

	// A partial message just before the wrap is superseded when a message
	// just after the wrap completes (raw `<=` would have kept it forever).
	std::vector<char> stale = MakeFrag(0xFFFFFFF0u, 0, 20, 10, 10);
	O3DS_CHECK(mapper.addFragment(1, stale.data(), stale.size(), 0));
	SendAll(mapper, 1, 2, "after wrap", 4, 0);
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)1); // the completed one
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == "after wrap");
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)0);
	O3DS_CHECK_EQ(mapper.stats().messagesSuperseded, (uint64_t)1);

	// Conversely, a partial message just after the wrap is NOT dropped when
	// an older (pre-wrap) id completes.
	std::vector<char> newer = MakeFrag(1, 0, 20, 10, 10);
	O3DS_CHECK(mapper.addFragment(1, newer.data(), newer.size(), 0));
	SendAll(mapper, 1, 0xFFFFFFFFu, "before wrap", 4, 0);
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == "before wrap");
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)1);
}

O3DS_TEST(UdpMapper_TwoSourcesSameId_ReassembleIndependently)
{
	UdpMapper mapper;
	std::vector<char> a0 = MakeFrag(77, 0, 20, 10, 10, 'a');
	std::vector<char> a1 = MakeFrag(77, 1, 20, 10, 10, 'a');
	std::vector<char> b0 = MakeFrag(77, 0, 30, 10, 10, 'b'); // different geometry
	std::vector<char> b1 = MakeFrag(77, 1, 30, 10, 10, 'b');
	std::vector<char> b2 = MakeFrag(77, 2, 30, 10, 10, 'b');

	O3DS_CHECK(mapper.addFragment(0xA, a0.data(), a0.size(), 0));
	O3DS_CHECK(mapper.addFragment(0xB, b0.data(), b0.size(), 0));
	O3DS_CHECK(mapper.addFragment(0xB, b1.data(), b1.size(), 0));
	O3DS_CHECK(mapper.addFragment(0xA, a1.data(), a1.size(), 0));
	O3DS_CHECK(mapper.addFragment(0xB, b2.data(), b2.size(), 0));

	std::vector<char> out;
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == std::string(20, 'a'));
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == std::string(30, 'b'));
	O3DS_CHECK(!mapper.getFrame(out));
}

// CORE-4: a rejected fragment for a new id must not create an entry, must
// not produce an empty frame, and must not drop other in-progress messages.
O3DS_TEST(UdpMapper_RejectedFragment_NoEmptyFrameAndKeepsOthers)
{
	UdpMapper mapper;
	std::vector<char> good0 = MakeFrag(5, 0, 2000, 1000, 1000, 'g');
	O3DS_CHECK(mapper.addFragment(1, good0.data(), good0.size(), 0));

	std::vector<char> bad = MakeFrag(9, 0, 0, 1000, 0); // bufSz == 0
	O3DS_CHECK(!mapper.addFragment(1, bad.data(), bad.size(), 0));
	std::vector<char> shortHdr(8, 0);
	O3DS_CHECK(!mapper.addFragment(1, shortHdr.data(), shortHdr.size(), 0));
	std::vector<char> badLen = MakeFrag(10, 0, 2000, 1000, 999); // short non-final
	O3DS_CHECK(!mapper.addFragment(1, badLen.data(), badLen.size(), 0));
	O3DS_CHECK_EQ(mapper.stats().fragmentsRejected, (uint64_t)3);

	std::vector<char> out;
	O3DS_CHECK(!mapper.getFrame(out));
	O3DS_CHECK(out.empty());
	O3DS_CHECK_EQ(mapper.inFlightMessages(), (size_t)1);

	std::vector<char> good1 = MakeFrag(5, 1, 2000, 1000, 1000, 'g');
	O3DS_CHECK(mapper.addFragment(1, good1.data(), good1.size(), 0));
	O3DS_CHECK(mapper.getFrame(out));
	O3DS_CHECK(AsString(out) == std::string(2000, 'g'));
}

// Rule of zero: copying a combiner that owns a buffer must not double-free.
O3DS_TEST(UdpCombiner_Copy_IsSafe)
{
	std::vector<char> f0 = MakeFrag(1, 0, 20, 10, 10, 'q');
	UdpCombiner a;
	O3DS_CHECK(a.addFragment(f0.data(), f0.size()));
	UdpCombiner b = a;
	UdpCombiner c;
	c = b;
	O3DS_CHECK_EQ(c.reservedBytes(), (size_t)20);
	O3DS_CHECK(!c.isComplete());
}
