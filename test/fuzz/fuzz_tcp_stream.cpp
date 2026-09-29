// Fuzz target: the TCP stream frame parser (src/o3ds/tcp_stream_parser.h),
// followed by SubjectList::Parse() of every frame it returns - the same
// pipeline the UE TCP receiver runs on the bytes it reads (WP-S6).
//
// Input:
//   [u8 maxSel]  max payload = 16 + maxSel * 64 bytes (16 .. 16336)
//   then `[u16 little-endian length][bytes]` records (SplitRecords), each
//   one "read" from the socket, at most kMaxReads.
//
// Checks:
//   - every frame is non-empty and within the max payload;
//   - bytes in = frame bytes + headers + discarded + still buffered;
//   - the buffer never holds more than one incomplete frame plus one read,
//     and its allocation stays within twice that;
//   - chunking does not matter: parsing the concatenated input in one
//     append returns exactly the same frames and discard count.

#include "fuzz_support.h"

#include "o3ds/model.h"
#include "o3ds/tcp_stream_parser.h"

#include <cstring>
#include <string>
#include <vector>

namespace
{
	const size_t kMaxReads = 256;

	// The payload sits 18 bytes into the parser's buffer, so it is copied
	// out first, as the UE receiver does before handing it to the consumer.
	// SubjectList::Parse() still reads its header words by type-punning
	// (CORE-22) and needs an aligned buffer.
	void ParseFrame(const uint8_t* data, size_t size)
	{
		const std::vector<char> copy(data, data + size);
		O3DS::SubjectList list;
		list.Parse(copy.data(), copy.size());
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	if (size < 1)
		return 0;

	const size_t maxPayload = 16 + (size_t)data[0] * 64;
	const std::vector<o3ds_fuzz::Record> reads = o3ds_fuzz::SplitRecords(data + 1, size - 1, kMaxReads);

	O3DS::TcpStreamParser parser(maxPayload);
	O3DS_FUZZ_ASSERT(parser.maxPayloadBytes() == maxPayload);

	std::vector<std::string> frames;
	std::vector<uint8_t> all;
	size_t bytesIn = 0;
	size_t frameBytes = 0;
	size_t largestRead = 0;

	for (const o3ds_fuzz::Record& read : reads)
	{
		// Request a little more than the read, as the receiver requests a
		// fixed chunk and gets back whatever recv() returns.
		uint8_t* out = parser.prepareWrite(read.size + 1);
		if (read.size > 0)
			std::memcpy(out, read.data, read.size);
		parser.commitWrite(read.size);
		all.insert(all.end(), read.data, read.data + read.size);
		bytesIn += read.size;
		largestRead = read.size + 1 > largestRead ? read.size + 1 : largestRead;

		const uint8_t* payload = nullptr;
		size_t payloadSize = 0;
		while (parser.next(payload, payloadSize))
		{
			O3DS_FUZZ_ASSERT(payloadSize > 0);
			O3DS_FUZZ_ASSERT(payloadSize <= maxPayload);
			frames.emplace_back(reinterpret_cast<const char*>(payload), payloadSize);
			frameBytes += payloadSize + O3DS::kTcpFrameHeaderSize;
			ParseFrame(payload, payloadSize);
		}

		const O3DS::TcpStreamParserStats& stats = parser.stats();
		O3DS_FUZZ_ASSERT(stats.frames == frames.size());
		O3DS_FUZZ_ASSERT(bytesIn == frameBytes + stats.discardedBytes + parser.buffered());
		// Nothing complete is left behind, so what's buffered is at most one
		// frame's header and payload.
		O3DS_FUZZ_ASSERT(parser.buffered() < O3DS::kTcpFrameHeaderSize + maxPayload);
		O3DS_FUZZ_ASSERT(parser.capacity() <= 2 * (O3DS::kTcpFrameHeaderSize + maxPayload + largestRead) + O3DS::TcpStreamParser::kRetainCapacity * 4);
	}

	// Chunking invariance.
	O3DS::TcpStreamParser whole(maxPayload);
	whole.append(all.data(), all.size());
	const uint8_t* payload = nullptr;
	size_t payloadSize = 0;
	size_t index = 0;
	while (whole.next(payload, payloadSize))
	{
		O3DS_FUZZ_ASSERT(index < frames.size());
		O3DS_FUZZ_ASSERT(frames[index].size() == payloadSize);
		O3DS_FUZZ_ASSERT(std::memcmp(frames[index].data(), payload, payloadSize) == 0);
		++index;
	}
	O3DS_FUZZ_ASSERT(index == frames.size());
	O3DS_FUZZ_ASSERT(whole.stats().discardedBytes == parser.stats().discardedBytes);
	O3DS_FUZZ_ASSERT(whole.stats().rejectedFrames == parser.stats().rejectedFrames);
	O3DS_FUZZ_ASSERT(whole.buffered() == parser.buffered());

	return 0;
}
