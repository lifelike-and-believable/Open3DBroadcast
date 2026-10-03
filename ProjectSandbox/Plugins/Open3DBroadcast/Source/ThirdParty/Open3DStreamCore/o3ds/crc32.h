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

#ifndef OPEN3D_STREAM_CRC32_H
#define OPEN3D_STREAM_CRC32_H

#include "o3ds_export.h"
#include <cstddef>
#include <cstdint>

namespace O3DS
{
	//! CRC-32 of the bytes at data: the parameters of CRCpp's CRC_32()
	//! (reflected, polynomial 0x04C11DB7, initial value and final XOR
	//! 0xFFFFFFFF), the checksum in every SubjectList buffer header. Gives
	//! exactly what CRCPP::CRC::Calculate(data, size, CRCPP::CRC::CRC_32())
	//! gives, so the wire format is unchanged, but reads eight bytes per step
	//! through lookup tables built once per process (slicing-by-8) instead of
	//! one bit at a time (CORE-7). Any alignment; any thread.
	O3DS_API std::uint32_t Crc32(const void* data, std::size_t size);
}

#endif // OPEN3D_STREAM_CRC32_H
