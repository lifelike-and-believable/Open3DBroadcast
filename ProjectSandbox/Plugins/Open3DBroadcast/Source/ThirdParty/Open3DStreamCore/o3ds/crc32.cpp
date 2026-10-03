/*
Open 3D Stream

Copyright 2026 Alastair Macleod

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "crc32.h"

#include <cstring>

namespace O3DS
{
	namespace
	{
		//! Reflected form of the CRC-32 polynomial 0x04C11DB7.
		constexpr std::uint32_t kCrc32Polynomial = 0xEDB88320u;

		//! Slicing-by-8 tables. Table[0] is the classic byte-at-a-time table;
		//! Table[k][b] is the CRC of byte b followed by k zero bytes, so
		//! eight table lookups advance the CRC by eight bytes.
		struct FCrc32Tables
		{
			std::uint32_t Table[8][256];
			bool bLittleEndian;

			FCrc32Tables()
			{
				for (std::uint32_t i = 0; i < 256; ++i)
				{
					std::uint32_t crc = i;
					for (int bit = 0; bit < 8; ++bit)
					{
						crc = (crc & 1u) ? (crc >> 1) ^ kCrc32Polynomial : (crc >> 1);
					}
					Table[0][i] = crc;
				}
				for (std::uint32_t i = 0; i < 256; ++i)
				{
					for (int k = 1; k < 8; ++k)
					{
						const std::uint32_t previous = Table[k - 1][i];
						Table[k][i] = (previous >> 8) ^ Table[0][previous & 0xFFu];
					}
				}
				const std::uint32_t probe = 1;
				unsigned char first = 0;
				std::memcpy(&first, &probe, 1);
				bLittleEndian = first == 1;
			}
		};

		const FCrc32Tables& Crc32Tables()
		{
			// Built on first use; C++11 makes a function-local static's
			// initialisation thread-safe.
			static const FCrc32Tables tables;
			return tables;
		}
	}

	std::uint32_t Crc32(const void* data, std::size_t size)
	{
		const FCrc32Tables& tables = Crc32Tables();
		const std::uint32_t (&t)[8][256] = tables.Table;
		const unsigned char* p = static_cast<const unsigned char*>(data);
		std::uint32_t crc = 0xFFFFFFFFu;

		// Eight bytes per step. The words are read with memcpy (any
		// alignment) and assume little-endian byte order; a big-endian host
		// takes the byte loop below for everything.
		if (tables.bLittleEndian)
		{
			while (size >= 8)
			{
				std::uint32_t one = 0;
				std::uint32_t two = 0;
				std::memcpy(&one, p, 4);
				std::memcpy(&two, p + 4, 4);
				one ^= crc;
				crc = t[7][one & 0xFFu] ^ t[6][(one >> 8) & 0xFFu] ^ t[5][(one >> 16) & 0xFFu] ^ t[4][one >> 24]
					^ t[3][two & 0xFFu] ^ t[2][(two >> 8) & 0xFFu] ^ t[1][(two >> 16) & 0xFFu] ^ t[0][two >> 24];
				p += 8;
				size -= 8;
			}
		}

		while (size > 0)
		{
			crc = (crc >> 8) ^ t[0][(crc ^ *p) & 0xFFu];
			++p;
			--size;
		}

		return crc ^ 0xFFFFFFFFu;
	}
}
