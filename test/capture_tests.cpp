// Acceptance tests for src/o3ds/capture.h (.o3dscap v1), per the roadmap
// doc's B1 spec (docs/roadmap/resilient-streaming-and-motion-prediction.md,
// section "Phase B1").
#include "test_framework.h"

#include "o3ds/capture.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace O3DS;

namespace
{
	std::vector<char> MakeWireBytes(char fill, size_t len)
	{
		return std::vector<char>(len, fill);
	}

	// Writes a raw little-endian uint32 directly into a stream, bypassing
	// the public API - used to hand-construct malformed/adversarial
	// captures that WriteCaptureRecord would never produce itself.
	void RawWriteU32LE(std::ostream& out, uint32_t v)
	{
		char buf[4];
		for (int i = 0; i < 4; i++)
			buf[i] = (char)((v >> (8 * i)) & 0xFF);
		out.write(buf, sizeof(buf));
	}

	void RawWriteU64LE(std::ostream& out, uint64_t v)
	{
		char buf[8];
		for (int i = 0; i < 8; i++)
			buf[i] = (char)((v >> (8 * i)) & 0xFF);
		out.write(buf, sizeof(buf));
	}
}

O3DS_TEST(Capture_HeaderRoundTrip)
{
	CaptureHeaderInfo written;
	written.flags = 1;
	written.base_wallclock_us = 1234567890123ull;
	written.schema_fingerprint = 0xDEADBEEF;
	written.predictor_version = 2;
	written.source_desc = "tcp://127.0.0.1:5555 test capture";

	std::ostringstream out;
	O3DS_CHECK(WriteCaptureHeader(out, written));

	std::istringstream in(out.str());
	CaptureHeaderInfo read;
	O3DS_CHECK(ReadCaptureHeader(in, read));

	O3DS_CHECK_EQ(read.flags, written.flags);
	O3DS_CHECK_EQ(read.base_wallclock_us, written.base_wallclock_us);
	O3DS_CHECK_EQ(read.schema_fingerprint, written.schema_fingerprint);
	O3DS_CHECK_EQ(read.predictor_version, written.predictor_version);
	O3DS_CHECK_EQ(read.source_desc, written.source_desc);
}

O3DS_TEST(Capture_HeaderRoundTrip_EmptySourceDesc)
{
	CaptureHeaderInfo written; // all defaults, empty source_desc
	std::ostringstream out;
	O3DS_CHECK(WriteCaptureHeader(out, written));

	std::istringstream in(out.str());
	CaptureHeaderInfo read;
	O3DS_CHECK(ReadCaptureHeader(in, read));
	O3DS_CHECK(read.source_desc.empty());
	O3DS_CHECK_EQ(read.base_wallclock_us, (uint64_t)0);
}

O3DS_TEST(Capture_WriteNFramesReadBackIdentical)
{
	std::ostringstream out;
	O3DS_CHECK(WriteCaptureHeader(out, CaptureHeaderInfo()));

	const int kFrameCount = 25;
	std::vector<CaptureRecord> written;
	for (int i = 0; i < kFrameCount; i++)
	{
		CaptureRecord r;
		r.recv_wallclock_us = 1000000ull + (uint64_t)i * 16667ull; // ~60fps spacing
		r.wire_bytes = MakeWireBytes((char)('A' + (i % 26)), (size_t)(i * 3 + 1)); // varying, non-trivial size
		O3DS_CHECK(WriteCaptureRecord(out, r));
		written.push_back(r);
	}

	std::istringstream in(out.str());
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header));

	int readCount = 0;
	CaptureRecord r;
	while (ReadCaptureRecord(in, r))
	{
		O3DS_CHECK(readCount < kFrameCount);
		O3DS_CHECK_EQ(r.recv_wallclock_us, written[readCount].recv_wallclock_us);
		O3DS_CHECK(r.wire_bytes == written[readCount].wire_bytes);
		readCount++;
	}

	O3DS_CHECK_EQ(readCount, kFrameCount); // exactly all of them, no more, no fewer
}

O3DS_TEST(Capture_EmptyWireBytesRecord_RoundTrips)
{
	std::ostringstream out;
	O3DS_CHECK(WriteCaptureHeader(out, CaptureHeaderInfo()));

	CaptureRecord written;
	written.recv_wallclock_us = 42;
	written.wire_bytes.clear();
	O3DS_CHECK(WriteCaptureRecord(out, written));

	std::istringstream in(out.str());
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header));

	CaptureRecord read;
	O3DS_CHECK(ReadCaptureRecord(in, read));
	O3DS_CHECK_EQ(read.recv_wallclock_us, (uint64_t)42);
	O3DS_CHECK(read.wire_bytes.empty());

	CaptureRecord none;
	O3DS_CHECK(ReadCaptureRecord(in, none) == false); // exactly one record, then clean end
}

O3DS_TEST(Capture_RejectsBadMagic)
{
	std::ostringstream out;
	out.write("NOTACAP\0", 8);
	// Rest of a well-formed-looking header, so only magic is wrong.
	std::string blob = out.str();
	blob.resize(64, '\0');

	std::istringstream in(blob);
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header) == false);
}

O3DS_TEST(Capture_RejectsUnknownFormatVersion)
{
	std::ostringstream out;
	CaptureHeaderInfo info;
	O3DS_CHECK(WriteCaptureHeader(out, info));
	std::string blob = out.str();

	// Overwrite the format_version field (offset 8, 2 bytes LE) with an
	// unsupported value.
	blob[8] = (char)0xFF;
	blob[9] = (char)0xFF;

	std::istringstream in(blob);
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header) == false);
}

O3DS_TEST(Capture_TruncatedHeader_RejectedAtEveryCutPoint)
{
	std::ostringstream out;
	CaptureHeaderInfo info;
	info.source_desc = "some source description";
	O3DS_CHECK(WriteCaptureHeader(out, info));
	std::string full = out.str();

	// A well-formed header must never partially parse: truncating at ANY
	// byte offset before the end must fail cleanly (no crash, ASan/UBSan
	// clean), not succeed with garbage/partial data.
	for (size_t cut = 0; cut < full.size(); cut++)
	{
		std::istringstream in(full.substr(0, cut));
		CaptureHeaderInfo header;
		bool ok = ReadCaptureHeader(in, header);
		O3DS_CHECK(ok == false);
	}
}

O3DS_TEST(Capture_TruncatedFinalRecord_StopsCleanlyNotError)
{
	// This is the core B1 guarantee: a capture cut mid-write (process
	// killed, disk full, etc.) must yield everything successfully written
	// before the cut, then stop without error - never crash, never throw.
	std::ostringstream out;
	O3DS_CHECK(WriteCaptureHeader(out, CaptureHeaderInfo()));

	CaptureRecord complete;
	complete.recv_wallclock_us = 100;
	complete.wire_bytes = MakeWireBytes('X', 10);
	O3DS_CHECK(WriteCaptureRecord(out, complete));

	CaptureRecord truncated;
	truncated.recv_wallclock_us = 200;
	truncated.wire_bytes = MakeWireBytes('Y', 50);
	O3DS_CHECK(WriteCaptureRecord(out, truncated));

	std::string full = out.str();

	// Cut off partway through the second record's wire_bytes.
	std::string cutBlob = full.substr(0, full.size() - 20);

	std::istringstream in(cutBlob);
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header));

	CaptureRecord r1;
	O3DS_CHECK(ReadCaptureRecord(in, r1));
	O3DS_CHECK_EQ(r1.recv_wallclock_us, (uint64_t)100);
	O3DS_CHECK(r1.wire_bytes == complete.wire_bytes);

	CaptureRecord r2;
	O3DS_CHECK(ReadCaptureRecord(in, r2) == false); // truncated - stop cleanly, no crash
}

O3DS_TEST(Capture_TruncatedRecordHeader_StopsCleanly)
{
	// Cut off mid-way through even the fixed 12-byte record header (before
	// wire_len is fully readable).
	std::ostringstream out;
	O3DS_CHECK(WriteCaptureHeader(out, CaptureHeaderInfo()));
	RawWriteU64LE(out, 999); // recv_wallclock_us, complete
	// wire_len (4 bytes) cut short after 2.
	out.put((char)0x01);
	out.put((char)0x02);

	std::istringstream in(out.str());
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header));

	CaptureRecord r;
	O3DS_CHECK(ReadCaptureRecord(in, r) == false);
}

O3DS_TEST(Capture_OversizedWireLen_RejectedWithoutHugeAllocation)
{
	// Hand-construct a record claiming a wire_len far beyond
	// kCaptureMaxWireLen, with no actual payload bytes following. A correct
	// reader must reject based on the length field alone, before attempting
	// to allocate/read - if it tried to honor this, it would attempt a
	// multi-gigabyte allocation for a payload that doesn't exist.
	std::ostringstream out;
	O3DS_CHECK(WriteCaptureHeader(out, CaptureHeaderInfo()));
	RawWriteU64LE(out, 1); // recv_wallclock_us
	RawWriteU32LE(out, 0xFFFFFFFFu); // wire_len: ~4GB, no data follows

	std::istringstream in(out.str());
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header));

	CaptureRecord r;
	O3DS_CHECK(ReadCaptureRecord(in, r) == false);
}

O3DS_TEST(Capture_WriterRejectsOversizedRecord)
{
	CaptureRecord huge;
	huge.wire_bytes.resize((size_t)kCaptureMaxWireLen + 1);

	std::ostringstream out;
	O3DS_CHECK(WriteCaptureRecord(out, huge) == false);
}

O3DS_TEST(Capture_ReaderTolerates_HeaderLenPadding)
{
	// A future/newer writer could add reserved padding between source_desc
	// and header_len (e.g. for forward-compatible extension fields). A v1
	// reader must skip straight to header_len, not assume records start
	// immediately after source_desc.
	std::ostringstream out;
	CaptureHeaderInfo info;
	info.source_desc = "x";
	O3DS_CHECK(WriteCaptureHeader(out, info));
	std::string blob = out.str();

	// header_len is at offset 10 (2 bytes LE). Bump it by 8 to simulate 8
	// bytes of reserved padding, and insert 8 zero bytes at that point.
	uint16_t originalHeaderLen = (uint16_t)((unsigned char)blob[10] | ((unsigned char)blob[11] << 8));
	uint16_t paddedHeaderLen = originalHeaderLen + 8;
	blob[10] = (char)(paddedHeaderLen & 0xFF);
	blob[11] = (char)((paddedHeaderLen >> 8) & 0xFF);
	blob.insert(originalHeaderLen, 8, '\0');

	CaptureRecord written;
	written.recv_wallclock_us = 7;
	written.wire_bytes = MakeWireBytes('Z', 3);
	std::ostringstream recordStream;
	O3DS_CHECK(WriteCaptureRecord(recordStream, written));
	blob += recordStream.str();

	std::istringstream in(blob);
	CaptureHeaderInfo header;
	O3DS_CHECK(ReadCaptureHeader(in, header));
	O3DS_CHECK_EQ(header.source_desc, std::string("x"));

	CaptureRecord read;
	O3DS_CHECK(ReadCaptureRecord(in, read));
	O3DS_CHECK_EQ(read.recv_wallclock_us, (uint64_t)7);
	O3DS_CHECK(read.wire_bytes == written.wire_bytes);
}

O3DS_TEST(Capture_RealFileRoundTrip)
{
	// Stringstream-based tests above prove the encoding logic; this proves
	// the actual file I/O path (open/write/flush/close/reopen/seek) works,
	// and that binary payload bytes (including 0x0A, the byte a text-mode
	// stream would be most likely to mangle) survive a real round trip.
	std::filesystem::path path = std::filesystem::temp_directory_path() / "o3ds_capture_test.o3dscap";

	CaptureRecord written;
	written.recv_wallclock_us = 555;
	written.wire_bytes = { (char)0x00, (char)0x0A, (char)0x0D, (char)0x0A, (char)0xFF, 'h', 'i' };

	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		O3DS_CHECK(out.is_open());
		CaptureHeaderInfo header;
		header.source_desc = "real file test";
		O3DS_CHECK(WriteCaptureHeader(out, header));
		O3DS_CHECK(WriteCaptureRecord(out, written));
	}

	{
		std::ifstream in(path, std::ios::binary);
		O3DS_CHECK(in.is_open());

		CaptureHeaderInfo header;
		O3DS_CHECK(ReadCaptureHeader(in, header));
		O3DS_CHECK_EQ(header.source_desc, std::string("real file test"));

		CaptureRecord read;
		O3DS_CHECK(ReadCaptureRecord(in, read));
		O3DS_CHECK_EQ(read.recv_wallclock_us, (uint64_t)555);
		O3DS_CHECK(read.wire_bytes == written.wire_bytes);

		CaptureRecord none;
		O3DS_CHECK(ReadCaptureRecord(in, none) == false);
	}

	std::filesystem::remove(path);
}
