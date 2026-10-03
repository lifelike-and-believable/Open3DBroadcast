// Tests for O3DS::Crc32 (src/o3ds/crc32.h, CORE-7): the slicing-by-8 CRC must
// give exactly the bitwise CRCpp CRC_32() result the SubjectList header has
// always carried, for every length, alignment and content, so the wire
// format does not change.
#include "test_framework.h"

#include "o3ds/crc32.h"
#include "o3ds/model.h"

#include "CRC.h"

#include <cstdint>
#include <cstring>
#include <vector>

using namespace O3DS;

namespace
{
	std::uint32_t BitwiseCrc32(const void* data, std::size_t size)
	{
		return CRCPP::CRC::Calculate(data, size, CRCPP::CRC::CRC_32());
	}

	//! Deterministic bytes (a fixed-seed LCG), so a failure reproduces.
	std::vector<unsigned char> Bytes(std::size_t size, std::uint32_t seed)
	{
		std::vector<unsigned char> out(size);
		std::uint32_t state = seed;
		for (std::size_t i = 0; i < size; ++i)
		{
			state = state * 1664525u + 1013904223u;
			out[i] = static_cast<unsigned char>(state >> 24);
		}
		return out;
	}
}

O3DS_TEST(Crc32_KnownCheckValue)
{
	// The CRC-32 check value: CRC of the ASCII digits "123456789".
	const char digits[] = "123456789";
	O3DS_CHECK(Crc32(digits, 9) == 0xCBF43926u);
	O3DS_CHECK(Crc32(nullptr, 0) == BitwiseCrc32(nullptr, 0));
}

O3DS_TEST(Crc32_MatchesBitwiseForEveryLengthAndAlignment)
{
	// Lengths 0 to 100 cover every tail after the 8-byte steps; offsets 0 to
	// 7 start the word loads at every alignment.
	const std::vector<unsigned char> buffer = Bytes(128, 12345u);
	for (std::size_t offset = 0; offset < 8; ++offset)
	{
		for (std::size_t length = 0; length <= 100; ++length)
		{
			const unsigned char* start = buffer.data() + offset;
			O3DS_CHECK(Crc32(start, length) == BitwiseCrc32(start, length));
		}
	}
}

O3DS_TEST(Crc32_MatchesBitwiseForLargeBuffers)
{
	const std::size_t sizes[] = { 1023, 4096, 34768, 65537, 1u << 20 };
	std::uint32_t seed = 1u;
	for (std::size_t size : sizes)
	{
		const std::vector<unsigned char> buffer = Bytes(size + 3, seed++);
		O3DS_CHECK(Crc32(buffer.data(), size) == BitwiseCrc32(buffer.data(), size));
		O3DS_CHECK(Crc32(buffer.data() + 3, size) == BitwiseCrc32(buffer.data() + 3, size));
	}
	const std::vector<unsigned char> zeros(4096, 0);
	const std::vector<unsigned char> ones(4096, 0xFF);
	O3DS_CHECK(Crc32(zeros.data(), zeros.size()) == BitwiseCrc32(zeros.data(), zeros.size()));
	O3DS_CHECK(Crc32(ones.data(), ones.size()) == BitwiseCrc32(ones.data(), ones.size()));
}

O3DS_TEST(Crc32_SerializedHeaderCarriesBitwiseCrc)
{
	// The header written by finalize() is what older readers check with the
	// bitwise CRC: flags 1, then the CRC of the payload.
	SubjectList list;
	Subject* subject = list.addSubject("crc");
	subject->addTransform("root", -1);
	subject->addTransform("child", 0);
	std::vector<char> buffer;
	O3DS_CHECK(list.Serialize(buffer, 1.0) > 8);

	std::uint32_t flags = 0;
	std::uint32_t crc = 0;
	std::memcpy(&flags, buffer.data(), 4);
	std::memcpy(&crc, buffer.data() + 4, 4);
	O3DS_CHECK(flags == 1u);
	O3DS_CHECK(crc == BitwiseCrc32(buffer.data() + 8, buffer.size() - 8));

	// A reused output buffer is rewritten, not appended to.
	const std::size_t firstSize = buffer.size();
	O3DS_CHECK(list.Serialize(buffer, 2.0) > 8);
	O3DS_CHECK(buffer.size() == firstSize);

	SubjectList parsed;
	O3DS_CHECK(parsed.Parse(buffer.data(), buffer.size()));
	O3DS_CHECK(parsed.size() == 1);
}
