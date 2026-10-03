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

	inline uint64_t LoadLE64(const void* p)
	{
		const uint8_t* b = static_cast<const uint8_t*>(p);
		return static_cast<uint64_t>(LoadLE32(b)) | (static_cast<uint64_t>(LoadLE32(b + 4)) << 32);
	}

	inline void StoreLE64(void* p, uint64_t v)
	{
		uint8_t* b = static_cast<uint8_t*>(p);
		StoreLE32(b, static_cast<uint32_t>(v));
		StoreLE32(b + 4, static_cast<uint32_t>(v >> 32));
	}

	// ---- Unified envelope (audio, control; ADR 0009 item 4) -----------------
	//
	// v2, written by every D8 writer, 24 bytes, little-endian:
	//   0-3   magic 'O','3','D','U' (a byte string)
	//   4     envelope version, 2
	//   5     kind (EnvelopeKind)
	//   6     codec (EnvelopeCodec)
	//   7     flags, 0
	//   8-15  timestamp_us, sender clock (ADR 0009 item 7)
	//   16-19 payload size
	//   20-23 seq, per stream and kind, wraps
	// v1, read during the compatibility window, 20 bytes, BIG-endian: magic
	// 'O','3','D','A', version 1, kind, codec, flags, u64 timestamp_us, u32
	// payload size, no seq.
	//
	// Kind and codec are passed through as read: each consumer checks the
	// pair it handles, and a reader ignores a kind it does not know (ADR 0011).

	constexpr size_t kEnvelopeV1HeaderSize = 20;
	constexpr size_t kEnvelopeV2HeaderSize = 24;
	constexpr uint8_t kEnvelopeVersion1 = 1;
	constexpr uint8_t kEnvelopeVersion2 = 2;

	enum class EnvelopeKind : uint8_t { Mocap = 0, Audio = 1, Control = 2 };
	enum class EnvelopeCodec : uint8_t { O3DS = 0, Opus = 1, PCM16 = 2, O3DControl = 3 };

	struct EnvelopeHeader
	{
		uint8_t version = 0;        //!< 1 or 2
		uint8_t kind = 0;
		uint8_t codec = 0;
		uint8_t flags = 0;
		uint64_t timestamp_us = 0;
		uint32_t payload_size = 0;
		uint32_t seq = 0;           //!< 0 for v1, which has none
		size_t header_size = 0;     //!< where the payload starts: 20 (v1) or 24 (v2)
	};

	//! True when data starts with an envelope magic, v1 ('O3DA') or v2 ('O3DU').
	O3DS_API bool HasEnvelopeMagic(const void* data, size_t len);

	//! Reads a v1 or v2 envelope header and checks that its payload fits in
	//! len (trailing bytes are allowed). A v2 header must have version 2 and
	//! flags 0. False for anything else.
	O3DS_API bool ReadEnvelopeHeader(const void* data, size_t len, EnvelopeHeader& out);

	//! Writes a v2 header into the first kEnvelopeV2HeaderSize bytes of out.
	O3DS_API void WriteEnvelopeHeaderV2(void* out, EnvelopeKind kind, EnvelopeCodec codec,
		uint64_t timestampUs, uint32_t payloadSize, uint32_t seq);

	//! Writes a v1 header (big-endian) into the first kEnvelopeV1HeaderSize
	//! bytes of out. Only for messages old readers must still recognise during
	//! the compatibility window (the TCP keepalive).
	O3DS_API void WriteEnvelopeHeaderV1(void* out, EnvelopeKind kind, EnvelopeCodec codec,
		uint64_t timestampUs, uint32_t payloadSize);

	//! Seconds on the sender clock as envelope microseconds: NaN, infinite or
	//! negative values become 0, values past the u64 range saturate (the
	//! plain cast was undefined for them, SHR-30).
	O3DS_API uint64_t EnvelopeTimestampUs(double seconds);
}
}
