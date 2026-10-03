// Fuzz target: SubjectList::PeekMeta(), the sequencing-metadata fast path
// ReorderGate callers run on every datagram before paying for a full
// Parse(). It sees the raw bytes first, and rejects them unless they pass
// the same frame checks as Parse() (CheckFrame: frame word, CRC, verifier;
// CORE-15, ADR 0009).
//
// Input: a whole wire buffer, header included.
//
// Invariant: PeekMeta() and Parse() run the same checks, so any buffer
// Parse() accepts (the input's payload with a valid header) must also be
// accepted by PeekMeta(), with the same metadata.
//
// The same holds for PeekPacketMeta() (WP-S4), which the UE receiver runs
// before either ordering path: a buffer Parse() accepts must not be dropped
// by it, and every subject Parse() reports as touched must be among the
// names it read (those names pick the sender stream).
#include "fuzz_support.h"

#include "o3ds/model.h"
#include "o3ds/receiver_streams.h"

#include <algorithm>

#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	uint64_t txSeq = 0;
	uint64_t wallclockUs = 0;
	uint32_t epoch = 0;
	const bool peeked = O3DS::SubjectList::PeekMeta(reinterpret_cast<const char*>(data), size, txSeq, wallclockUs, epoch);
	if (!peeked)
	{
		O3DS_FUZZ_ASSERT(txSeq == 0 && wallclockUs == 0 && epoch == 0);
	}

	O3DS::PacketMeta meta;
	if (!O3DS::PeekPacketMeta(reinterpret_cast<const char*>(data), size, meta))
	{
		O3DS_FUZZ_ASSERT(meta.tx_seq == 0 && meta.stream_key == 0 && meta.subject_names.empty());
	}

	if (size < 8)
		return 0;

	// Same bytes with a valid header, so Parse() gets past the CRC.
	const std::vector<char> wire = o3ds_fuzz::WrapPayload(data + 8, size - 8);
	O3DS::SubjectList list;
	std::vector<O3DS::ParsedSubjectInfo> touched;
	if (list.Parse(wire.data(), wire.size(), nullptr, true, &touched))
	{
		// The raw input peeks only if its own header was already valid.
		uint64_t wireTxSeq = 0;
		uint64_t wireWallclockUs = 0;
		uint32_t wireEpoch = 0;
		O3DS_FUZZ_ASSERT(O3DS::SubjectList::PeekMeta(wire.data(), wire.size(), wireTxSeq, wireWallclockUs, wireEpoch));
		if (peeked)
		{
			O3DS_FUZZ_ASSERT(wireTxSeq == txSeq && wireWallclockUs == wallclockUs && wireEpoch == epoch);
		}
		txSeq = wireTxSeq;
		wallclockUs = wireWallclockUs;
		epoch = wireEpoch;

		O3DS::PacketMeta wireMeta;
		O3DS_FUZZ_ASSERT(O3DS::PeekPacketMeta(wire.data(), wire.size(), wireMeta));
		O3DS_FUZZ_ASSERT(wireMeta.tx_seq == txSeq && wireMeta.frame_epoch == epoch);
		for (const O3DS::ParsedSubjectInfo& info : touched)
		{
			O3DS_FUZZ_ASSERT(std::find(wireMeta.subject_names.begin(), wireMeta.subject_names.end(), info.name) != wireMeta.subject_names.end());
		}

		uint64_t txSeq2 = 0;
		uint64_t wallclockUs2 = 0;
		uint32_t epoch2 = 0;
		O3DS_FUZZ_ASSERT(O3DS::SubjectList::PeekMeta(wire.data(), wire.size(), txSeq2, wallclockUs2, epoch2));
		O3DS_FUZZ_ASSERT(txSeq2 == txSeq && wallclockUs2 == wallclockUs && epoch2 == epoch);
	}

	return 0;
}
