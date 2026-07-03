// Tests for src/o3ds/sequencing.h and the tx_seq/tx_wallclock_us/frame_epoch
// wire fields (SubjectList::Serialize/SerializeUpdate/PeekMeta).
#include "test_framework.h"

#include "o3ds/model.h"
#include "o3ds/sequencing.h"

#include <cstring>

using namespace O3DS;

O3DS_TEST(SequenceCounterIsMonotonicAndStartsAtOne)
{
	SequenceCounter counter;
	uint64_t a = counter.Next();
	uint64_t b = counter.Next();
	uint64_t c = counter.Next();

	O3DS_CHECK_EQ(a, (uint64_t)1);
	O3DS_CHECK_EQ(b, (uint64_t)2);
	O3DS_CHECK_EQ(c, (uint64_t)3);
}

O3DS_TEST(SequenceCounterResetReturnsToOne)
{
	SequenceCounter counter;
	counter.Next();
	counter.Next();
	counter.Reset();

	O3DS_CHECK_EQ(counter.Next(), (uint64_t)1);
}

O3DS_TEST(NowUtcMicrosIsPlausibleAndAdvances)
{
	uint64_t t1 = NowUtcMicros();
	uint64_t t2 = NowUtcMicros();

	// Sanity bound: any time after ~2020-01-01 in microseconds. Catches a
	// unit mistake (e.g. accidentally returning milliseconds or seconds)
	// without pinning down an exact value.
	O3DS_CHECK(t1 > 1577836800000000ull);
	O3DS_CHECK(t2 >= t1);
}

O3DS_TEST(TxSeqAndWallclockRoundTripThroughSerializeAndParse)
{
	SubjectList subjects;
	subjects.addSubject("Performer")->addTransform("Root", -1);

	std::vector<char> buffer;
	int size = subjects.Serialize(buffer, /*timestamp*/ 1.0,
		/*tx_seq*/ 42, /*tx_wallclock_us*/ 1234567890123ull, /*frame_epoch*/ 7);
	O3DS_CHECK(size > 0);

	uint64_t peekedSeq = 0, peekedWallclock = 0;
	uint32_t peekedEpoch = 0;
	bool peeked = SubjectList::PeekMeta(buffer.data(), buffer.size(), peekedSeq, peekedWallclock, peekedEpoch);
	O3DS_CHECK(peeked);
	O3DS_CHECK_EQ(peekedSeq, (uint64_t)42);
	O3DS_CHECK_EQ(peekedWallclock, (uint64_t)1234567890123ull);
	O3DS_CHECK_EQ(peekedEpoch, (uint32_t)7);

	// A full Parse() must also see the subject data as usual - the new
	// fields are additive, not a replacement for the existing payload.
	SubjectList parsed;
	bool ok = parsed.Parse(buffer.data(), buffer.size());
	O3DS_CHECK(ok);
	O3DS_CHECK(parsed.findSubject("Performer") != nullptr);
}

O3DS_TEST(UnsetTxFieldsDefaultToZeroLikeALegacySender)
{
	SubjectList subjects;
	subjects.addSubject("Legacy")->addTransform("Root", -1);

	std::vector<char> buffer;
	// Deliberately omit tx_seq/tx_wallclock_us/frame_epoch - matches how an
	// old sender that predates these fields would call Serialize().
	int size = subjects.Serialize(buffer, /*timestamp*/ 1.0);
	O3DS_CHECK(size > 0);

	uint64_t peekedSeq = 1, peekedWallclock = 1; // pre-seed with non-zero to prove they get zeroed
	uint32_t peekedEpoch = 1;
	bool peeked = SubjectList::PeekMeta(buffer.data(), buffer.size(), peekedSeq, peekedWallclock, peekedEpoch);
	O3DS_CHECK(peeked);
	O3DS_CHECK_EQ(peekedSeq, (uint64_t)0);
	O3DS_CHECK_EQ(peekedWallclock, (uint64_t)0);
	O3DS_CHECK_EQ(peekedEpoch, (uint32_t)0);
}

O3DS_TEST(PeekMetaRejectsShortAndMalformedBuffers)
{
	uint64_t seq = 0, wallclock = 0;
	uint32_t epoch = 0;

	// Too short to even contain the 8-byte header.
	char tiny[4] = { 0, 0, 0, 0 };
	O3DS_CHECK(SubjectList::PeekMeta(tiny, 4, seq, wallclock, epoch) == false);
	O3DS_CHECK(SubjectList::PeekMeta(tiny, 0, seq, wallclock, epoch) == false);
	O3DS_CHECK(SubjectList::PeekMeta(nullptr, 100, seq, wallclock, epoch) == false);

	// Long enough for the header, but garbage FlatBuffers payload - must
	// fail the Verifier rather than reading out-of-bounds or garbage data.
	char garbage[32];
	memset(garbage, 0xFF, sizeof(garbage));
	O3DS_CHECK(SubjectList::PeekMeta(garbage, sizeof(garbage), seq, wallclock, epoch) == false);
	O3DS_CHECK_EQ(seq, (uint64_t)0);
}
