// Fuzz target: UDP fragment reassembly (UdpMapper, and UdpCombiner on its
// own), followed by SubjectList::Parse() of every frame it hands back -
// the same pipeline the UE UDP receiver runs on each datagram.
//
// Input: a sequence of datagram records, at most kMaxDatagrams:
//   [24-byte v2 fragment header, as on the wire: magic, version, u32 id, seq, bufSz, fragSize]
//   [u8 lenMode]
//   [u8 ctl]
//   [payload bytes, only when lenMode >= 0x80]
// lenMode < 0x80: the payload has the "honest" length the header implies
//   (fragSize, or the tail for the last fragment; capped at the largest
//   datagram addFragment() accepts), filled with the matching slice of
//   CanonicalKeyframe(). A correctly fragmented seed therefore reassembles
//   into a valid O3DS buffer, and mutating only header fields keeps the
//   payload length consistent with them - which is what reaches the
//   interesting checks instead of stopping at the length test.
// lenMode >= 0x80: the payload is the next (lenMode & 0x7f) input bytes.
// ctl bits 0-1: the sender (UdpMapper sourceKey, 0-3).
// ctl bits 2-7: advance the clock by that many 4 ms steps (0-252 ms) before
//   this datagram, so the 100 ms message timeout is reachable.
//
// After every datagram the mapper's documented bounds are checked: in-flight
// message count and reserved bytes stay within UdpReassemblyConfig, and
// getFrame() never returns an empty frame.

#include "fuzz_support.h"

#include "o3ds/model.h"
#include "o3ds/udp_fragment.h"

#include <algorithm>

namespace
{
	// Bounds the work per input; UdpMapper itself bounds memory (WP-S2).
	const size_t kMaxDatagrams = 64;
	const size_t kHeaderSize = kUdpFragmentHeaderSize;
	const size_t kRecordPrefix = kHeaderSize + 2; // header, lenMode, ctl

	void ParseFrame(const char* data, size_t size)
	{
		O3DS::SubjectList list;
		list.Parse(data, size);
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::vector<char>& message = o3ds_fuzz::CanonicalKeyframe();

	// Smaller than the defaults so the per-message, in-flight and total-byte
	// limits are all reachable within kMaxDatagrams records, and so each run
	// doesn't zero-fill 4 MiB buffers. The limit logic is the same code
	// whatever the numbers are.
	UdpReassemblyConfig smallConfig;
	smallConfig.maxMessageSize = 256 * 1024;
	smallConfig.maxInFlightMessages = 4;
	smallConfig.maxTotalBytes = 512 * 1024;
	UdpMapper mapper(smallConfig);
	const UdpReassemblyConfig& config = mapper.config();
	UdpCombiner combiner(config.maxMessageSize);
	uint64_t nowMs = 0;
	std::vector<char> datagram;
	std::vector<char> frame;

	size_t pos = 0;
	for (size_t n = 0; n < kMaxDatagrams && pos + kRecordPrefix <= size; ++n)
	{
		UdpFragmentHeader header;
		readUdpFragmentHeader(reinterpret_cast<const char*>(data) + pos, kHeaderSize, header);
		const uint8_t lenMode = data[pos + kHeaderSize];
		const uint8_t ctl = data[pos + kHeaderSize + 1];
		const uint64_t sourceKey = ctl & 0x3;
		nowMs += (uint64_t)(ctl >> 2) * 4;

		datagram.assign(reinterpret_cast<const char*>(data) + pos,
			reinterpret_cast<const char*>(data) + pos + kHeaderSize);
		pos += kRecordPrefix;

		if (lenMode >= 0x80)
		{
			const size_t len = std::min<size_t>(lenMode & 0x7f, size - pos);
			datagram.insert(datagram.end(), data + pos, data + pos + len);
			pos += len;
		}
		else
		{
			const uint64_t seq = header.seq;
			const uint64_t bufSz = header.totalSize;
			const uint64_t fragSize = header.fragSize;
			const uint64_t start = seq * fragSize;
			uint64_t len = fragSize;
			if (fragSize != 0 && start < bufSz && bufSz - start < fragSize)
				len = bufSz - start; // last fragment: the tail
			len = std::min<uint64_t>(len, kUdpMaxDatagramSize - kHeaderSize);

			const size_t base = datagram.size();
			datagram.resize(base + (size_t)len);
			for (uint64_t i = 0; i < len; ++i)
			{
				const uint64_t src = start + i;
				datagram[base + (size_t)i] = src < message.size() ? message[(size_t)src] : (char)(src & 0xff);
			}
		}

		mapper.expire(nowMs);
		mapper.addFragment(sourceKey, datagram.data(), datagram.size(), nowMs);
		O3DS_FUZZ_ASSERT(mapper.inFlightMessages() <= config.maxInFlightMessages);
		O3DS_FUZZ_ASSERT(mapper.bytesInUse() <= config.maxTotalBytes);

		for (size_t drained = 0; drained < kMaxDatagrams; ++drained)
		{
			if (!mapper.getFrame(frame))
				break;
			O3DS_FUZZ_ASSERT(!frame.empty());
			ParseFrame(frame.data(), frame.size());
		}

		combiner.addFragment(datagram.data(), datagram.size());
	}

	if (combiner.isComplete())
	{
		O3DS_FUZZ_ASSERT(combiner.mBuffer.size() >= combiner.mBufferSize);
		ParseFrame(combiner.mBuffer.data(), combiner.mBufferSize);
	}

	return 0;
}
