#pragma once

#include "CoreMinimal.h"

namespace O3DSockets::Tcp
{
	/** Shared TCP framing helpers used by the sockets sender/receiver implementations. */
	inline constexpr int32 FrameMagicSize = 14;
	inline constexpr int32 FrameHeaderSize = FrameMagicSize + sizeof(uint32);

	inline constexpr uint8 FrameMagic[FrameMagicSize] =
	{
		0x00, 0xFF, 0x03, 0xFE, 'O', '3', 'D', 'S', '-', 'S', 'T', 'A', 'R', 'T'
	};

	inline void WriteFrameHeader(uint8* Destination, int32 PayloadSize)
	{
		check(Destination != nullptr);
		check(PayloadSize >= 0);

		FMemory::Memcpy(Destination, FrameMagic, FrameMagicSize);
		const uint32 Size = static_cast<uint32>(PayloadSize);
		Destination[FrameMagicSize + 0] = static_cast<uint8>(Size & 0xFF);
		Destination[FrameMagicSize + 1] = static_cast<uint8>((Size >> 8) & 0xFF);
		Destination[FrameMagicSize + 2] = static_cast<uint8>((Size >> 16) & 0xFF);
		Destination[FrameMagicSize + 3] = static_cast<uint8>((Size >> 24) & 0xFF);
	}

	inline bool MatchesMagic(const uint8* Buffer)
	{
		check(Buffer != nullptr);
		for (int32 Index = 0; Index < FrameMagicSize; ++Index)
		{
			if (Buffer[Index] != FrameMagic[Index])
			{
				return false;
			}
		}
		return true;
	}

	inline uint32 DecodePayloadSize(const uint8* Buffer)
	{
		check(Buffer != nullptr);
		return static_cast<uint32>(Buffer[FrameMagicSize]) |
			(static_cast<uint32>(Buffer[FrameMagicSize + 1]) << 8) |
			(static_cast<uint32>(Buffer[FrameMagicSize + 2]) << 16) |
			(static_cast<uint32>(Buffer[FrameMagicSize + 3]) << 24);
	}
}
