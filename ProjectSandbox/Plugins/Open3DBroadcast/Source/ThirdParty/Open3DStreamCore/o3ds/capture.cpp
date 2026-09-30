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

#include "capture.h"

#include <cstring>

namespace
{
	// Fixed portion of the header, before the variable-length source_desc
	// tail: magic(8) + format_version(2) + header_len(2) + flags(4) +
	// base_wallclock_us(8) + schema_fingerprint(4) + predictor_version(4) +
	// source_desc_len(2) = 34 bytes.
	const size_t kHeaderFixedSize = 34;
	const size_t kMaxSourceDescLen = 65535 - kHeaderFixedSize; // so header_len still fits in uint16
	const char kMagic[8] = { 'O', '3', 'D', 'S', 'C', 'A', 'P', '\0' };

	// Explicit little-endian encode/decode: correct regardless of host byte
	// order (never relies on raw memcpy of a multi-byte integer).

	void WriteU16LE(std::ostream& out, uint16_t v)
	{
		char buf[2] = { (char)(v & 0xFF), (char)((v >> 8) & 0xFF) };
		out.write(buf, sizeof(buf));
	}

	void WriteU32LE(std::ostream& out, uint32_t v)
	{
		char buf[4];
		for (int i = 0; i < 4; i++)
			buf[i] = (char)((v >> (8 * i)) & 0xFF);
		out.write(buf, sizeof(buf));
	}

	void WriteU64LE(std::ostream& out, uint64_t v)
	{
		char buf[8];
		for (int i = 0; i < 8; i++)
			buf[i] = (char)((v >> (8 * i)) & 0xFF);
		out.write(buf, sizeof(buf));
	}

	bool ReadU16LE(std::istream& in, uint16_t& outValue)
	{
		unsigned char buf[2];
		in.read((char*)buf, sizeof(buf));
		if (!in || in.gcount() != (std::streamsize)sizeof(buf))
			return false;
		outValue = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
		return true;
	}

	bool ReadU32LE(std::istream& in, uint32_t& outValue)
	{
		unsigned char buf[4];
		in.read((char*)buf, sizeof(buf));
		if (!in || in.gcount() != (std::streamsize)sizeof(buf))
			return false;
		outValue = 0;
		for (int i = 0; i < 4; i++)
			outValue |= ((uint32_t)buf[i]) << (8 * i);
		return true;
	}

	bool ReadU64LE(std::istream& in, uint64_t& outValue)
	{
		unsigned char buf[8];
		in.read((char*)buf, sizeof(buf));
		if (!in || in.gcount() != (std::streamsize)sizeof(buf))
			return false;
		outValue = 0;
		for (int i = 0; i < 8; i++)
			outValue |= ((uint64_t)buf[i]) << (8 * i);
		return true;
	}
}

namespace O3DS
{
	bool WriteCaptureHeader(std::ostream& out, const CaptureHeaderInfo& info)
	{
		if (info.source_desc.size() > kMaxSourceDescLen)
			return false;

		uint16_t sourceDescLen = (uint16_t)info.source_desc.size();
		uint16_t headerLen = (uint16_t)(kHeaderFixedSize + sourceDescLen);

		out.write(kMagic, sizeof(kMagic));
		WriteU16LE(out, kCaptureFormatVersion);
		WriteU16LE(out, headerLen);
		WriteU32LE(out, info.flags);
		WriteU64LE(out, info.base_wallclock_us);
		WriteU32LE(out, info.schema_fingerprint);
		WriteU32LE(out, info.predictor_version);
		WriteU16LE(out, sourceDescLen);
		if (sourceDescLen > 0)
			out.write(info.source_desc.data(), sourceDescLen);

		return (bool)out;
	}

	bool WriteCaptureRecord(std::ostream& out, const CaptureRecord& record)
	{
		if (record.wire_bytes.size() > kCaptureMaxWireLen)
			return false;

		WriteU64LE(out, record.recv_wallclock_us);
		WriteU32LE(out, (uint32_t)record.wire_bytes.size());
		if (!record.wire_bytes.empty())
			out.write(record.wire_bytes.data(), (std::streamsize)record.wire_bytes.size());

		return (bool)out;
	}

	bool ReadCaptureHeader(std::istream& in, CaptureHeaderInfo& outInfo)
	{
		outInfo = CaptureHeaderInfo();

		char magic[8];
		in.read(magic, sizeof(magic));
		if (!in || in.gcount() != (std::streamsize)sizeof(magic))
			return false;
		if (memcmp(magic, kMagic, sizeof(kMagic)) != 0)
			return false;

		uint16_t formatVersion = 0;
		if (!ReadU16LE(in, formatVersion))
			return false;
		if (formatVersion != kCaptureFormatVersion) // v1 reader: no minor-version tolerance yet
			return false;

		uint16_t headerLen = 0;
		if (!ReadU16LE(in, headerLen))
			return false;
		if (headerLen < kHeaderFixedSize)
			return false; // too small to even hold the fixed fields - corrupt

		uint32_t flags = 0;
		uint64_t baseWallclockUs = 0;
		uint32_t schemaFingerprint = 0;
		uint32_t predictorVersion = 0;
		uint16_t sourceDescLen = 0;
		if (!ReadU32LE(in, flags)) return false;
		if (!ReadU64LE(in, baseWallclockUs)) return false;
		if (!ReadU32LE(in, schemaFingerprint)) return false;
		if (!ReadU32LE(in, predictorVersion)) return false;
		if (!ReadU16LE(in, sourceDescLen)) return false;

		if ((size_t)headerLen < kHeaderFixedSize + sourceDescLen)
			return false; // declared header too small to hold its own source_desc - corrupt

		std::string sourceDesc;
		if (sourceDescLen > 0)
		{
			sourceDesc.resize(sourceDescLen);
			in.read(&sourceDesc[0], sourceDescLen);
			if (!in || in.gcount() != (std::streamsize)sourceDescLen)
				return false;
		}

		// Skip any reserved padding a newer writer may have appended between
		// source_desc and header_len, so records start at the right offset
		// regardless of header_len exceeding what we understood how to parse.
		in.seekg((std::streamoff)headerLen, std::ios::beg);
		if (!in)
			return false;

		outInfo.flags = flags;
		outInfo.base_wallclock_us = baseWallclockUs;
		outInfo.schema_fingerprint = schemaFingerprint;
		outInfo.predictor_version = predictorVersion;
		outInfo.source_desc = std::move(sourceDesc);
		return true;
	}

	bool ReadCaptureRecord(std::istream& in, CaptureRecord& outRecord)
	{
		uint64_t recvWallclockUs = 0;
		if (!ReadU64LE(in, recvWallclockUs))
			return false; // clean EOF or truncated - both mean "no more records"

		uint32_t wireLen = 0;
		if (!ReadU32LE(in, wireLen))
			return false; // truncated mid-record-header - stop cleanly, not an error

		if (wireLen > kCaptureMaxWireLen)
			return false; // corrupt/adversarial length - reject before allocating

		std::vector<char> wireBytes;
		if (wireLen > 0)
		{
			wireBytes.resize(wireLen);
			in.read(wireBytes.data(), wireLen);
			if (!in || in.gcount() != (std::streamsize)wireLen)
				return false; // truncated final record - stop cleanly, not an error
		}

		outRecord.recv_wallclock_us = recvWallclockUs;
		outRecord.wire_bytes = std::move(wireBytes);
		return true;
	}
}
