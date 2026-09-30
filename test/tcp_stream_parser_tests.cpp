// Unit tests for src/o3ds/tcp_stream_parser.h, the socket-free TCP frame
// parser used by the UE Sockets receiver (WP-S6: TRB-1, TRB-8, TRB-9;
// ADR 0006 S2).
#include "test_framework.h"

#include "o3ds/tcp_stream_parser.h"

#include <cstring>
#include <string>
#include <vector>

using O3DS::TcpStreamParser;

namespace
{
	std::vector<uint8_t> Frame(const std::string& payload)
	{
		std::vector<uint8_t> out(O3DS::kTcpFrameHeaderSize + payload.size());
		O3DS::writeTcpFrameHeader(out.data(), static_cast<uint32_t>(payload.size()));
		std::memcpy(out.data() + O3DS::kTcpFrameHeaderSize, payload.data(), payload.size());
		return out;
	}

	std::vector<uint8_t> Header(uint32_t length)
	{
		std::vector<uint8_t> out(O3DS::kTcpFrameHeaderSize);
		O3DS::writeTcpFrameHeader(out.data(), length);
		return out;
	}

	void Append(TcpStreamParser& parser, const std::vector<uint8_t>& bytes)
	{
		parser.append(bytes.data(), bytes.size());
	}

	void Concat(std::vector<uint8_t>& out, const std::vector<uint8_t>& more)
	{
		out.insert(out.end(), more.begin(), more.end());
	}

	// Pops every complete frame currently buffered.
	std::vector<std::string> Drain(TcpStreamParser& parser)
	{
		std::vector<std::string> frames;
		const uint8_t* payload = nullptr;
		size_t size = 0;
		while (parser.next(payload, size))
			frames.emplace_back(reinterpret_cast<const char*>(payload), size);
		return frames;
	}
}

O3DS_TEST(TcpStreamParser_HeaderLayout_MatchesWire)
{
	// 14-byte magic then the little-endian length, as the UE sender and the
	// MotionBuilder device write it.
	const std::vector<uint8_t> h = Header(0x01020304u);
	const uint8_t expected[] = { 0x00, 0xFF, 0x03, 0xFE, 'O', '3', 'D', 'S', '-', 'S', 'T', 'A', 'R', 'T', 0x04, 0x03, 0x02, 0x01 };
	O3DS_CHECK_EQ(h.size(), sizeof(expected));
	O3DS_CHECK(std::memcmp(h.data(), expected, sizeof(expected)) == 0);
}

O3DS_TEST(TcpStreamParser_ThreeFramesInOneRead_AllReturned)
{
	// TRB-1: the old receiver kept only the first frame of a read.
	std::vector<uint8_t> bytes = Frame("alpha");
	Concat(bytes, Frame("bravo-bravo"));
	Concat(bytes, Frame("c"));

	TcpStreamParser parser;
	Append(parser, bytes);
	const std::vector<std::string> frames = Drain(parser);
	O3DS_CHECK_EQ(frames.size(), 3u);
	O3DS_CHECK(frames[0] == "alpha");
	O3DS_CHECK(frames[1] == "bravo-bravo");
	O3DS_CHECK(frames[2] == "c");
	O3DS_CHECK_EQ(parser.buffered(), 0u);
	O3DS_CHECK_EQ(parser.stats().frames, 3u);
	O3DS_CHECK_EQ(parser.stats().discardedBytes, 0u);
}

O3DS_TEST(TcpStreamParser_HeaderSplitAcrossReads_FrameReturnedOnce)
{
	const std::vector<uint8_t> bytes = Frame("split-header-payload");
	for (size_t cut = 1; cut < bytes.size(); ++cut)
	{
		TcpStreamParser parser;
		parser.append(bytes.data(), cut);
		const uint8_t* payload = nullptr;
		size_t size = 0;
		O3DS_CHECK(!parser.next(payload, size));
		parser.append(bytes.data() + cut, bytes.size() - cut);
		const std::vector<std::string> frames = Drain(parser);
		O3DS_CHECK_EQ(frames.size(), 1u);
		O3DS_CHECK(frames[0] == "split-header-payload");
		O3DS_CHECK_EQ(parser.stats().discardedBytes, 0u);
	}
}

O3DS_TEST(TcpStreamParser_ByteAtATime_EveryFrameReturned)
{
	std::vector<uint8_t> bytes;
	for (int i = 0; i < 20; ++i)
		Concat(bytes, Frame("frame-" + std::to_string(i)));

	TcpStreamParser parser;
	std::vector<std::string> frames;
	for (uint8_t b : bytes)
	{
		parser.append(&b, 1);
		for (const std::string& f : Drain(parser))
			frames.push_back(f);
	}
	O3DS_CHECK_EQ(frames.size(), 20u);
	for (int i = 0; i < 20; ++i)
		O3DS_CHECK(frames[static_cast<size_t>(i)] == "frame-" + std::to_string(i));
	O3DS_CHECK_EQ(parser.stats().discardedBytes, 0u);
}

O3DS_TEST(TcpStreamParser_GarbageThenMagic_ResyncsInOnePass)
{
	// TRB-8: garbage used to drain one byte per Poll(). Here 100 KiB of
	// garbage (with partial magics in it) is skipped in a single next().
	std::vector<uint8_t> bytes;
	for (size_t i = 0; i < 100 * 1024; ++i)
		bytes.push_back(static_cast<uint8_t>((i * 131) & 0xFF));
	// A partial magic and a magic prefix followed by the wrong byte.
	const uint8_t partial[] = { 0x00, 0xFF, 0x03, 0xFE, 'O', '3', 'D' };
	bytes.insert(bytes.end(), partial, partial + sizeof(partial));
	const size_t garbageSize = bytes.size();
	Concat(bytes, Frame("after-garbage"));
	Concat(bytes, Frame("second"));

	TcpStreamParser parser;
	Append(parser, bytes);
	const uint8_t* payload = nullptr;
	size_t size = 0;
	O3DS_CHECK(parser.next(payload, size));
	O3DS_CHECK(std::string(reinterpret_cast<const char*>(payload), size) == "after-garbage");
	O3DS_CHECK(parser.next(payload, size));
	O3DS_CHECK(std::string(reinterpret_cast<const char*>(payload), size) == "second");
	O3DS_CHECK(!parser.next(payload, size));
	O3DS_CHECK_EQ(parser.stats().discardedBytes, garbageSize);
	O3DS_CHECK_EQ(parser.stats().resyncs, 1u);
}

O3DS_TEST(TcpStreamParser_GarbageOnly_KeepsAtMostAPartialMagic)
{
	std::vector<uint8_t> bytes(64 * 1024, 0x5A);
	bytes.push_back(0x00);
	bytes.push_back(0xFF); // could be the start of a magic: kept

	TcpStreamParser parser;
	Append(parser, bytes);
	O3DS_CHECK(Drain(parser).empty());
	O3DS_CHECK_EQ(parser.buffered(), 2u);
	O3DS_CHECK_EQ(parser.stats().discardedBytes, 64u * 1024u);

	// The rest of a frame whose magic started with those two bytes.
	const std::vector<uint8_t> frame = Frame("joined");
	parser.append(frame.data() + 2, frame.size() - 2);
	const std::vector<std::string> frames = Drain(parser);
	O3DS_CHECK_EQ(frames.size(), 1u);
	O3DS_CHECK(frames[0] == "joined");
}

O3DS_TEST(TcpStreamParser_OversizeFrame_RejectedAndStreamRecovers)
{
	// TRB-9: a length above the limit is rejected without allocating it.
	TcpStreamParser parser(1024);
	std::vector<uint8_t> bytes = Header(1025);
	bytes.resize(bytes.size() + 200, 0x11); // part of the oversize "payload"
	Concat(bytes, Frame("small-enough"));

	Append(parser, bytes);
	const std::vector<std::string> frames = Drain(parser);
	O3DS_CHECK_EQ(frames.size(), 1u);
	O3DS_CHECK(frames[0] == "small-enough");
	O3DS_CHECK_EQ(parser.stats().rejectedFrames, 1u);
	O3DS_CHECK(parser.capacity() < 64u * 1024u);
}

O3DS_TEST(TcpStreamParser_HugeAnnouncedLength_DoesNotAllocateIt)
{
	// Default limit 4 MiB; a header announcing 50 MiB is rejected, and one
	// announcing 3 MiB only grows the buffer as bytes actually arrive.
	TcpStreamParser parser;
	Append(parser, Header(50u * 1024u * 1024u));
	O3DS_CHECK(Drain(parser).empty());
	O3DS_CHECK_EQ(parser.stats().rejectedFrames, 1u);

	Append(parser, Header(3u * 1024u * 1024u));
	parser.append(reinterpret_cast<const uint8_t*>("xyz"), 3);
	O3DS_CHECK(Drain(parser).empty());
	O3DS_CHECK(parser.capacity() < 64u * 1024u);
}

O3DS_TEST(TcpStreamParser_ZeroLength_TreatedAsGarbage)
{
	TcpStreamParser parser;
	std::vector<uint8_t> bytes = Header(0);
	Concat(bytes, Frame("ok"));
	Append(parser, bytes);
	const std::vector<std::string> frames = Drain(parser);
	O3DS_CHECK_EQ(frames.size(), 1u);
	O3DS_CHECK(frames[0] == "ok");
	O3DS_CHECK_EQ(parser.stats().rejectedFrames, 1u);
}

O3DS_TEST(TcpStreamParser_LargeFrame_BufferShrinksAfterwards)
{
	TcpStreamParser parser;
	const std::string big(3u * 1024u * 1024u, 'b');
	const std::vector<uint8_t> bytes = Frame(big);

	// Feed through prepareWrite/commitWrite in 64 KiB reads, as the receiver does.
	size_t offset = 0;
	std::vector<std::string> frames;
	while (offset < bytes.size())
	{
		const size_t chunk = std::min<size_t>(64 * 1024, bytes.size() - offset);
		uint8_t* out = parser.prepareWrite(64 * 1024);
		std::memcpy(out, bytes.data() + offset, chunk);
		parser.commitWrite(chunk);
		offset += chunk;
		for (const std::string& f : Drain(parser))
			frames.push_back(f);
	}
	O3DS_CHECK_EQ(frames.size(), 1u);
	O3DS_CHECK(frames[0] == big);
	O3DS_CHECK(parser.capacity() >= big.size());
	O3DS_CHECK(parser.capacity() <= 2u * (big.size() + O3DS::kTcpFrameHeaderSize + 64u * 1024u));

	// The next small read releases the large buffer.
	Append(parser, Frame("small"));
	O3DS_CHECK(parser.capacity() <= TcpStreamParser::kRetainCapacity);
	frames = Drain(parser);
	O3DS_CHECK_EQ(frames.size(), 1u);
	O3DS_CHECK(frames[0] == "small");
}

O3DS_TEST(TcpStreamParser_PartialFrameSurvivesCompaction)
{
	// Frames straddling reads while earlier frames are consumed: the live
	// bytes are moved to the front and the partial frame stays intact.
	TcpStreamParser parser;
	std::vector<uint8_t> all;
	for (int i = 0; i < 200; ++i)
		Concat(all, Frame(std::string(static_cast<size_t>(1000 + i), static_cast<char>('a' + (i % 26)))));

	size_t offset = 0;
	int received = 0;
	while (offset < all.size())
	{
		const size_t chunk = std::min<size_t>(777, all.size() - offset);
		uint8_t* out = parser.prepareWrite(4096);
		std::memcpy(out, all.data() + offset, chunk);
		parser.commitWrite(chunk);
		offset += chunk;
		for (const std::string& f : Drain(parser))
		{
			O3DS_CHECK_EQ(f.size(), static_cast<size_t>(1000 + received));
			O3DS_CHECK(f[0] == static_cast<char>('a' + (received % 26)));
			++received;
		}
	}
	O3DS_CHECK_EQ(received, 200);
	O3DS_CHECK_EQ(parser.stats().discardedBytes, 0u);
}

O3DS_TEST(TcpStreamParser_Reset_DropsPartialFrame)
{
	TcpStreamParser parser;
	const std::vector<uint8_t> frame = Frame("interrupted");
	parser.append(frame.data(), frame.size() - 3);
	parser.reset();
	O3DS_CHECK_EQ(parser.buffered(), 0u);
	O3DS_CHECK_EQ(parser.capacity(), 0u);
	Append(parser, Frame("fresh"));
	const std::vector<std::string> frames = Drain(parser);
	O3DS_CHECK_EQ(frames.size(), 1u);
	O3DS_CHECK(frames[0] == "fresh");
}

O3DS_TEST(TcpStreamParser_MaxPayload_Clamped)
{
	TcpStreamParser parser(0);
	O3DS_CHECK_EQ(parser.maxPayloadBytes(), 1u);
	parser.setMaxPayloadBytes(1000u * 1024u * 1024u);
	O3DS_CHECK_EQ(parser.maxPayloadBytes(), O3DS::kTcpMaxPayloadLimit);
}
