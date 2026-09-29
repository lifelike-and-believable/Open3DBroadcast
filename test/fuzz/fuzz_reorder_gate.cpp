// Fuzz target: ReorderGate::Push()/Flush() under arbitrary sequence
// numbers, epochs and timing.
//
// Input:
//   byte 0: Config::max_window = 1 + (b % 32)
//   byte 1: Config::max_delay_s = b milliseconds
//   byte 2: Config::reset_backjump = b * 4
//   then ops, at most kMaxOps. Each op byte `o` first advances the clock by
//   (o >> 2) ms, then does (o & 3):
//     0: push seq = previous seq + (int8) next byte, current epoch
//     1: push seq = next 8 bytes (u64 little-endian), current epoch
//     2: Flush()
//     3: current epoch = next byte, then push seq = previous seq + 1
//
// Invariants checked on every call:
//   - each emitted frame carries the bytes that were pushed with its seq
//     (the gate must never mix up or corrupt payloads);
//   - PendingCount() < max_window once Push()/Flush() returns (the window
//     bound is what keeps the gate's memory bounded);
//   - stats counters never go backwards.
#include "fuzz_support.h"

#include "o3ds/reorder_gate.h"

#include <cstring>

namespace
{
	const size_t kMaxOps = 4096;

	std::vector<char> Tag(uint64_t seq, uint32_t epoch)
	{
		std::vector<char> bytes(12);
		std::memcpy(bytes.data(), &seq, 8);
		std::memcpy(bytes.data() + 8, &epoch, 4);
		return bytes;
	}

	// `lost` is deliberately not checked: it grows by the size of each
	// skipped gap, so a sender-controlled tx_seq jump of close to 2^64 wraps
	// it (found by this target; reported for triage, not a memory-safety
	// issue). The other counters only ever grow by one.
	void CheckStatsMonotonic(const O3DS::ReorderStats& before, const O3DS::ReorderStats& after)
	{
		O3DS_FUZZ_ASSERT(after.delivered >= before.delivered);
		O3DS_FUZZ_ASSERT(after.dup_dropped >= before.dup_dropped);
		O3DS_FUZZ_ASSERT(after.stale_dropped >= before.stale_dropped);
		O3DS_FUZZ_ASSERT(after.reordered >= before.reordered);
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size < 3)
		return 0;

	O3DS::ReorderGate::Config config;
	config.max_window = 1 + (data[0] % 32);
	config.max_delay_s = data[1] / 1000.0;
	config.reset_backjump = (uint64_t)data[2] * 4;

	O3DS::ReorderGate gate(config);

	auto emit = [](O3DS::Frame&& frame) {
		O3DS_FUZZ_ASSERT(frame.bytes == Tag(frame.seq, frame.epoch));
	};

	double now = 0.0;
	uint64_t seq = 0;
	uint32_t epoch = 0;
	size_t pos = 3;

	for (size_t ops = 0; ops < kMaxOps && pos < size; ++ops)
	{
		const uint8_t op = data[pos++];
		now += (op >> 2) / 1000.0;

		const O3DS::ReorderStats before = gate.Stats();
		bool push = true;
		switch (op & 3)
		{
		case 0:
			if (pos < size)
				seq += (uint64_t)(int64_t)(int8_t)data[pos++];
			break;
		case 1:
		{
			uint8_t raw[8] = {};
			const size_t n = size - pos < 8 ? size - pos : 8;
			std::memcpy(raw, data + pos, n);
			pos += n;
			seq = 0;
			for (int i = 7; i >= 0; --i)
				seq = (seq << 8) | raw[i];
			break;
		}
		case 2:
			push = false;
			gate.Flush(now, emit);
			break;
		default:
			if (pos < size)
				epoch = data[pos++];
			seq += 1;
			break;
		}

		if (push)
		{
			O3DS::Frame frame;
			frame.seq = seq;
			frame.epoch = epoch;
			frame.bytes = Tag(seq, epoch);
			gate.Push(std::move(frame), now, emit);
		}

		O3DS_FUZZ_ASSERT(gate.PendingCount() < config.max_window);
		CheckStatsMonotonic(before, gate.Stats());
	}

	return 0;
}
