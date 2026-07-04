// Acceptance tests for src/o3ds/udp_fragment.h's UdpCombiner reassembly,
// covering the short-final-fragment hardening fix (a malformed last fragment
// must not be accepted with fewer bytes than the reassembled buffer expects -
// UdpCombiner's mBuffer is malloc'd, not zeroed, so accepting it would let
// isComplete()/getFrame() hand back uninitialized memory to the caller).
#include "test_framework.h"

#include "o3ds/udp_fragment.h"

#include <cstring>
#include <vector>

namespace
{
	// Builds a valid, well-formed fragment for a given (id, seq, bufSz,
	// fragSize, payload) using the same HEADERSIZE=16 layout addFragment()
	// expects: four little/native-endian uint32_t's (id, seq, bufSz,
	// fragSize) followed by the payload bytes.
	std::vector<char> MakeFragment(uint32_t id, uint32_t seq, uint32_t bufSz, uint32_t fragSize, const char* payload, size_t payloadLen)
	{
		std::vector<char> out;
		uint32_t heading[4] = { id, seq, bufSz, fragSize };
		out.insert(out.end(), (char*)heading, (char*)heading + 16);
		out.insert(out.end(), payload, payload + payloadLen);
		return out;
	}
}

O3DS_TEST(UdpFragment_RoundTrip_ReassemblesExactly)
{
	const char* msg = "the quick brown fox jumps over the lazy dog";
	size_t msgLen = strlen(msg);
	UdpFragmenter fragmenter(msg, msgLen, 10);

	UdpCombiner combiner;
	for (uint32_t seq = 0; seq < (uint32_t)fragmenter.mFrames; seq++)
	{
		std::vector<char> frag;
		fragmenter.makeFragment(1, seq, frag);
		O3DS_CHECK(combiner.addFragment(frag.data(), frag.size()));
	}

	O3DS_CHECK(combiner.isComplete());
	O3DS_CHECK_EQ(combiner.mBufferSize, msgLen);
	O3DS_CHECK(memcmp(combiner.mBuffer, msg, msgLen) == 0);
}

O3DS_TEST(UdpFragment_ShortFinalFragment_IsRejected)
{
	// bufSz=25, fragSize=10 -> 3 fragments (10, 10, 5). Feed a final fragment
	// with only 3 payload bytes instead of the required 5 - this used to
	// pass (writeEnd=25 <= bufSz=25) and leave the last 2 bytes of mBuffer
	// uninitialized while still marking mFound[2]=true.
	std::vector<char> f0 = MakeFragment(1, 0, 25, 10, "0123456789", 10);
	std::vector<char> f1 = MakeFragment(1, 1, 25, 10, "abcdefghij", 10);
	std::vector<char> shortFinal = MakeFragment(1, 2, 25, 10, "xyz", 3);

	UdpCombiner combiner;
	O3DS_CHECK(combiner.addFragment(f0.data(), f0.size()));
	O3DS_CHECK(combiner.addFragment(f1.data(), f1.size()));
	O3DS_CHECK(!combiner.addFragment(shortFinal.data(), shortFinal.size()));
	O3DS_CHECK(!combiner.isComplete());
}

O3DS_TEST(UdpFragment_ExactFinalFragment_IsAccepted)
{
	// Same shape as above, but the final fragment carries exactly the
	// expected 5 remaining bytes - must still succeed.
	std::vector<char> f0 = MakeFragment(1, 0, 25, 10, "0123456789", 10);
	std::vector<char> f1 = MakeFragment(1, 1, 25, 10, "abcdefghij", 10);
	std::vector<char> f2 = MakeFragment(1, 2, 25, 10, "klmno", 5);

	UdpCombiner combiner;
	O3DS_CHECK(combiner.addFragment(f0.data(), f0.size()));
	O3DS_CHECK(combiner.addFragment(f1.data(), f1.size()));
	O3DS_CHECK(combiner.addFragment(f2.data(), f2.size()));
	O3DS_CHECK(combiner.isComplete());
	O3DS_CHECK(memcmp(combiner.mBuffer, "0123456789abcdefghijklmno", 25) == 0);
}

O3DS_TEST(UdpFragment_ShortNonFinalFragment_IsRejected)
{
	// A non-final fragment shorter than fragSize was already rejected before
	// this fix (the `sz != fragSize + HEADERSIZE` check); confirm it still
	// is under the rewritten check.
	std::vector<char> shortNonFinal = MakeFragment(1, 0, 25, 10, "01234", 5);

	UdpCombiner combiner;
	O3DS_CHECK(!combiner.addFragment(shortNonFinal.data(), shortNonFinal.size()));
}
