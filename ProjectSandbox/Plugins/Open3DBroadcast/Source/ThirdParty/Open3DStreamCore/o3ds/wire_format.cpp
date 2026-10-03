#include "wire_format.h"

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
}
}
