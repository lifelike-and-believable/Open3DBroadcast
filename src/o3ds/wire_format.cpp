#include "wire_format.h"

#include <cmath>
#include <cstring>

namespace O3DS
{
namespace Wire
{
	const char* ToString(FrameCheck check)
	{
		switch (check)
		{
		case FrameCheck::Ok: return "ok";
		case FrameCheck::TooShort: return "buffer too short";
		case FrameCheck::BadFrameWord: return "invalid frame word";
		case FrameCheck::VersionTooNew: return "sender requires a newer protocol; update this receiver";
		case FrameCheck::CrcMismatch: return "CRC check failed";
		case FrameCheck::VerifyFailed: return "FlatBuffers verification failed";
		case FrameCheck::UndeclaredNewContent: return "residual or quantized content in a protocol-1 frame (pre-D8 sender)";
		}
		return "unknown";
	}

	FrameCheck ReadFrameHeader(const char* data, size_t len, uint8_t& outMinReaderVersion, uint32_t& outCrc)
	{
		outMinReaderVersion = 0;
		outCrc = 0;
		if (data == nullptr || len < kFrameHeaderSize)
		{
			return FrameCheck::TooShort;
		}

		const uint32_t word = LoadLE32(data);
		const uint8_t minReader = static_cast<uint8_t>(word & 0xFFu);
		if ((word & 0xFFFFFF00u) != 0 || minReader == 0)
		{
			return FrameCheck::BadFrameWord;
		}
		outMinReaderVersion = minReader;
		if (minReader > kProtocolVersion)
		{
			return FrameCheck::VersionTooNew;
		}
		outCrc = LoadLE32(data + 4);
		return FrameCheck::Ok;
	}

	namespace
	{
		const uint8_t kMagicV1[4] = { 'O', '3', 'D', 'A' };
		const uint8_t kMagicV2[4] = { 'O', '3', 'D', 'U' };

		uint32_t LoadBE32(const uint8_t* b)
		{
			return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16)
				| (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
		}

		void StoreBE32(uint8_t* b, uint32_t v)
		{
			b[0] = static_cast<uint8_t>(v >> 24);
			b[1] = static_cast<uint8_t>(v >> 16);
			b[2] = static_cast<uint8_t>(v >> 8);
			b[3] = static_cast<uint8_t>(v);
		}
	}

	bool HasEnvelopeMagic(const void* data, size_t len)
	{
		if (data == nullptr || len < 4)
			return false;
		return std::memcmp(data, kMagicV1, 4) == 0 || std::memcmp(data, kMagicV2, 4) == 0;
	}

	bool ReadEnvelopeHeader(const void* data, size_t len, EnvelopeHeader& out)
	{
		out = EnvelopeHeader();
		if (data == nullptr || len < 4)
			return false;
		const uint8_t* b = static_cast<const uint8_t*>(data);
		EnvelopeHeader h;
		if (std::memcmp(b, kMagicV2, 4) == 0)
		{
			if (len < kEnvelopeV2HeaderSize || b[4] != kEnvelopeVersion2 || b[7] != 0)
				return false;
			h.version = b[4];
			h.kind = b[5];
			h.codec = b[6];
			h.flags = b[7];
			h.timestamp_us = LoadLE64(b + 8);
			h.payload_size = LoadLE32(b + 16);
			h.seq = LoadLE32(b + 20);
			h.header_size = kEnvelopeV2HeaderSize;
		}
		else if (std::memcmp(b, kMagicV1, 4) == 0)
		{
			if (len < kEnvelopeV1HeaderSize)
				return false;
			h.version = b[4];
			h.kind = b[5];
			h.codec = b[6];
			h.flags = b[7];
			h.timestamp_us = (static_cast<uint64_t>(LoadBE32(b + 8)) << 32) | LoadBE32(b + 12);
			h.payload_size = LoadBE32(b + 16);
			h.header_size = kEnvelopeV1HeaderSize;
		}
		else
		{
			return false;
		}
		if (static_cast<uint64_t>(h.header_size) + h.payload_size > len)
			return false;
		out = h;
		return true;
	}

	void WriteEnvelopeHeaderV2(void* out, EnvelopeKind kind, EnvelopeCodec codec,
		uint64_t timestampUs, uint32_t payloadSize, uint32_t seq)
	{
		uint8_t* b = static_cast<uint8_t*>(out);
		std::memcpy(b, kMagicV2, 4);
		b[4] = kEnvelopeVersion2;
		b[5] = static_cast<uint8_t>(kind);
		b[6] = static_cast<uint8_t>(codec);
		b[7] = 0;
		StoreLE64(b + 8, timestampUs);
		StoreLE32(b + 16, payloadSize);
		StoreLE32(b + 20, seq);
	}

	void WriteEnvelopeHeaderV1(void* out, EnvelopeKind kind, EnvelopeCodec codec,
		uint64_t timestampUs, uint32_t payloadSize)
	{
		uint8_t* b = static_cast<uint8_t*>(out);
		std::memcpy(b, kMagicV1, 4);
		b[4] = kEnvelopeVersion1;
		b[5] = static_cast<uint8_t>(kind);
		b[6] = static_cast<uint8_t>(codec);
		b[7] = 0;
		StoreBE32(b + 8, static_cast<uint32_t>(timestampUs >> 32));
		StoreBE32(b + 12, static_cast<uint32_t>(timestampUs));
		StoreBE32(b + 16, payloadSize);
	}

	uint64_t EnvelopeTimestampUs(double seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0.0)
			return 0;
		const double us = seconds * 1.0e6;
		// 2^64 as a double; anything at or above it would overflow the cast.
		if (us >= 18446744073709551616.0)
			return UINT64_MAX;
		return static_cast<uint64_t>(us);
	}

	namespace
	{
		uint64_t FnvBytes(uint64_t hash, const void* data, size_t len)
		{
			const uint8_t* p = static_cast<const uint8_t*>(data);
			for (size_t i = 0; i < len; ++i)
			{
				hash ^= p[i];
				hash *= 1099511628211ull;
			}
			return hash;
		}

		uint64_t FnvLE32(uint64_t hash, uint32_t v)
		{
			uint8_t b[4];
			StoreLE32(b, v);
			return FnvBytes(hash, b, 4);
		}
	}

	uint64_t HashNamesBegin(uint32_t count)
	{
		return FnvLE32(kFnv64OffsetBasis, count);
	}

	uint64_t HashNamesAdd(uint64_t hash, const char* utf8, uint32_t len)
	{
		hash = FnvLE32(hash, len);
		return len > 0 && utf8 != nullptr ? FnvBytes(hash, utf8, len) : hash;
	}

	uint64_t HashNames(const std::vector<std::string>& names)
	{
		uint64_t hash = HashNamesBegin(static_cast<uint32_t>(names.size()));
		for (const std::string& name : names)
		{
			hash = HashNamesAdd(hash, name.data(), static_cast<uint32_t>(name.size()));
		}
		return hash;
	}

	uint64_t HashParents(uint64_t hash, const int32_t* parents, uint32_t count)
	{
		hash = FnvLE32(hash, count);
		for (uint32_t i = 0; i < count; ++i)
		{
			hash = FnvLE32(hash, static_cast<uint32_t>(parents[i]));
		}
		return hash;
	}
}
}
