#pragma once

// Wire-format constants and helpers shared by every O3DS frame reader and
// writer (D8, docs/adr/0009-protocol-versioning.md). Everything on the wire
// is little-endian; reads and writes go through LoadLE*/StoreLE*, never a
// type-punned pointer (CORE-22).

#include "o3ds_export.h"

#include <cstddef>
#include <cstdint>

//! The wire protocol this build implements (ADR 0009 item 2). Readers accept
//! frames whose min_reader_version is 1..O3DS_PROTOCOL_VERSION; writers stamp
//! it into SubjectList.protocol_version. Bump rules: ADR 0009 item 9.
#define O3DS_PROTOCOL_VERSION 2

namespace O3DS
{
namespace Wire
{
	constexpr uint8_t kProtocolVersion = O3DS_PROTOCOL_VERSION;

	//! Frames that any reader since protocol 1 may apply: legacy full
	//! snapshots and non-quantized, non-residual deltas.
	constexpr uint8_t kMinReaderPlain = 1;
	//! Frames carrying residual (predictor_id != 0) or quantized (*_q8 /
	//! *_q16) content, which a pre-D8 reader would misapply.
	constexpr uint8_t kMinReaderResidualOrQuantized = 2;

	//! A frame is an 8-byte header (frame word, CRC-32 of the payload) and
	//! then the FlatBuffers payload.
	constexpr size_t kFrameHeaderSize = 8;

	inline uint32_t LoadLE32(const void* p)
	{
		const uint8_t* b = static_cast<const uint8_t*>(p);
		return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8)
			| (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
	}

	inline void StoreLE32(void* p, uint32_t v)
	{
		uint8_t* b = static_cast<uint8_t*>(p);
		b[0] = static_cast<uint8_t>(v);
		b[1] = static_cast<uint8_t>(v >> 8);
		b[2] = static_cast<uint8_t>(v >> 16);
		b[3] = static_cast<uint8_t>(v >> 24);
	}

	//! The frame word: byte 0 is min_reader_version; bytes 1 (flags) and 2-3
	//! (reserved) are 0 (ADR 0009 item 1). Version 1 has the same bytes as the
	//! pre-D8 flags word 0x00000001.
	inline uint32_t MakeFrameWord(uint8_t minReaderVersion)
	{
		return static_cast<uint32_t>(minReaderVersion);
	}

	//! Why a frame was rejected, or Ok.
	enum class FrameCheck : uint8_t
	{
		Ok = 0,
		TooShort,             //!< shorter than the 8-byte header
		BadFrameWord,         //!< min_reader_version 0, or a flag or reserved byte set
		VersionTooNew,        //!< min_reader_version above O3DS_PROTOCOL_VERSION
		CrcMismatch,          //!< payload CRC-32 differs from the header
		VerifyFailed,         //!< not a well-formed SubjectList (or no identifier on a version-2 frame)
		UndeclaredNewContent, //!< min_reader_version 1 but residual or quantized content (pre-D8 develop writer)
	};

	//! A short English description, for logs.
	O3DS_API const char* ToString(FrameCheck check);

	//! Reads the frame word and stored CRC. Checks only the length and the
	//! frame word (version range, zero flag and reserved bytes); the CRC and
	//! the payload are the caller's (CheckFrame in model.h does both).
	O3DS_API FrameCheck ReadFrameHeader(const char* data, size_t len, uint8_t& outMinReaderVersion, uint32_t& outCrc);
}
}
