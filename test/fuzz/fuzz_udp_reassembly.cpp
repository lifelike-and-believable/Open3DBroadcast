// Fuzz target: UDP fragment reassembly (UdpMapper, and UdpCombiner on its
// own), followed by SubjectList::Parse() of every frame it hands back -
// the same pipeline the UE UDP receiver runs on each datagram.
//
// Input: a sequence of datagram records, at most kMaxDatagrams:
//   [16-byte fragment header, as on the wire: u32 id, seq, bufSz, fragSize]
//   [u8 lenMode]
//   [payload bytes, only when lenMode >= 0x80]
// lenMode < 0x80: the payload has the "honest" length the header implies
//   (fragSize, or the tail for the last fragment; capped at the largest
//   datagram addFragment() accepts), filled with the matching slice of
//   CanonicalKeyframe(). A correctly fragmented seed therefore reassembles
//   into a valid O3DS buffer, and mutating only header fields keeps the
//   payload length consistent with them - which is what reaches the
//   interesting checks instead of stopping at the length test.
// lenMode >= 0x80: the payload is the next (lenMode & 0x7f) input bytes.
//
// kMaxDatagrams bounds how many distinct message ids one input can open.
// Each can reserve up to 64 MB today (CORE-3, fixed separately in WP-S2);
// without the cap one input could legitimately exhaust the fuzzer's RSS
// limit and mask every other finding.
#include "fuzz_support.h"

#include "o3ds/model.h"
#include "o3ds/udp_fragment.h"

#include <algorithm>
#include <cstring>

namespace
{
	const size_t kMaxDatagrams = 64;
	const size_t kHeaderSize = 16;
	const size_t kMaxDatagramSize = 65511; // addFragment()'s own upper bound

	void ParseFrame(const char* data, size_t size)
	{
		O3DS::SubjectList list;
		list.Parse(data, size);
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::vector<char>& message = o3ds_fuzz::CanonicalKeyframe();

	UdpMapper mapper;
	UdpCombiner combiner;
	std::vector<char> datagram;
	std::vector<char> frame;

	size_t pos = 0;
	for (size_t n = 0; n < kMaxDatagrams && pos + kHeaderSize + 1 <= size; ++n)
	{
		uint32_t header[4];
		std::memcpy(header, data + pos, kHeaderSize);
		const uint8_t lenMode = data[pos + kHeaderSize];
		pos += kHeaderSize + 1;

		datagram.assign(reinterpret_cast<const char*>(data) + pos - kHeaderSize - 1,
			reinterpret_cast<const char*>(data) + pos - 1);

		if (lenMode >= 0x80)
		{
			const size_t len = std::min<size_t>(lenMode & 0x7f, size - pos);
			datagram.insert(datagram.end(), data + pos, data + pos + len);
			pos += len;
		}
		else
		{
			const uint64_t seq = header[1];
			const uint64_t bufSz = header[2];
			const uint64_t fragSize = header[3];
			const uint64_t start = seq * fragSize;
			uint64_t len = fragSize;
			if (fragSize != 0 && start < bufSz && bufSz - start < fragSize)
				len = bufSz - start; // last fragment: the tail
			len = std::min<uint64_t>(len, kMaxDatagramSize - kHeaderSize);

			const size_t base = datagram.size();
			datagram.resize(base + (size_t)len);
			for (uint64_t i = 0; i < len; ++i)
			{
				const uint64_t src = start + i;
				datagram[base + (size_t)i] = src < message.size() ? message[(size_t)src] : (char)(src & 0xff);
			}
		}

		mapper.addFragment(datagram.data(), datagram.size());
		for (size_t drained = 0; drained < kMaxDatagrams; ++drained)
		{
			frame.clear();
			if (!mapper.getFrame(frame))
				break;
			ParseFrame(frame.data(), frame.size());
		}

		combiner.addFragment(datagram.data(), datagram.size());
	}

	if (combiner.mBufferSize > 0 && combiner.isComplete())
		ParseFrame(combiner.mBuffer, combiner.mBufferSize);

	return 0;
}
