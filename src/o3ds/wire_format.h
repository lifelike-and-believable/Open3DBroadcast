#pragma once

// Wire-format constants and helpers shared by every O3DS frame reader and
// writer (D8, docs/adr/0009-protocol-versioning.md). Everything on the wire
// is little-endian; reads and writes go through LoadLE*/StoreLE*, never a
// type-punned pointer (CORE-22).

#include "o3ds_export.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
	// Envelope v2, 24 bytes, little-endian:
	//   0-3   magic 'O','3','D','U' (a byte string)
	//   4     envelope version, 2
	//   5     kind (EnvelopeKind)
	//   6     codec (EnvelopeCodec)
	//   7     flags, 0
	//   8-15  timestamp_us, sender clock (ADR 0009 item 7)
	//   16-19 payload size
	//   20-23 seq, per stream and kind, wraps
	// Envelope v1 ('O3DA', big-endian) is not accepted: no deployed reader or
	// writer uses it (the maintainer confirmed there are no old receivers).
	//
	// Kind and codec are passed through as read: each consumer checks the
	// pair it handles, and a reader ignores a kind it does not know (ADR 0011).

	constexpr size_t kEnvelopeV2HeaderSize = 24;
	constexpr uint8_t kEnvelopeVersion2 = 2;

	enum class EnvelopeKind : uint8_t { Mocap = 0, Audio = 1, Control = 2 };
	enum class EnvelopeCodec : uint8_t { O3DS = 0, Opus = 1, PCM16 = 2, O3DControl = 3 };

	struct EnvelopeHeader
	{
		uint8_t version = 0;        //!< 2
		uint8_t kind = 0;
		uint8_t codec = 0;
		uint8_t flags = 0;
		uint64_t timestamp_us = 0;
		uint32_t payload_size = 0;
		uint32_t seq = 0;
		size_t header_size = 0;     //!< where the payload starts (kEnvelopeV2HeaderSize)
	};

	//! True when data starts with the envelope magic 'O3DU'.
	O3DS_API bool HasEnvelopeMagic(const void* data, size_t len);

	//! Reads an envelope header and checks that its payload fits in len
	//! (trailing bytes are allowed). It must have version 2 and flags 0.
	//! False for anything else.
	O3DS_API bool ReadEnvelopeHeader(const void* data, size_t len, EnvelopeHeader& out);

	//! Writes a v2 header into the first kEnvelopeV2HeaderSize bytes of out.
	O3DS_API void WriteEnvelopeHeaderV2(void* out, EnvelopeKind kind, EnvelopeCodec codec,
		uint64_t timestampUs, uint32_t payloadSize, uint32_t seq);

	//! Seconds on the sender clock as envelope microseconds: NaN, infinite or
	//! negative values become 0, values past the u64 range saturate (the
	//! plain cast was undefined for them, SHR-30).
	O3DS_API uint64_t EnvelopeTimestampUs(double seconds);

	// ---- Name hashing (ADR 0009 item 8, SHR-33) ------------------------------
	//
	// 64-bit FNV-1a over the element count (u32 LE), then for each name its
	// UTF-8 length (u32 LE) and exact UTF-8 bytes. Case is preserved. The
	// length prefixes make {"ab","c"} and {"a","bc"} hash differently. Local
	// today (change detection); a topology hash that goes on the wire must use
	// this definition and bump the protocol version.

	constexpr uint64_t kFnv64OffsetBasis = 1469598103934665603ull;

	//! Starts a names hash for count names.
	O3DS_API uint64_t HashNamesBegin(uint32_t count);
	//! Adds one name (UTF-8, len bytes) to a names hash.
	O3DS_API uint64_t HashNamesAdd(uint64_t hash, const char* utf8, uint32_t len);
	//! HashNamesBegin, then HashNamesAdd for every name.
	O3DS_API uint64_t HashNames(const std::vector<std::string>& names);
	//! Continues a hash with parent indices: their count (u32 LE), then each as i32 LE.
	O3DS_API uint64_t HashParents(uint64_t hash, const int32_t* parents, uint32_t count);
}

	//! The sender's engine timecode for one frame (SubjectList.scene_time;
	//! RCV-8, ADR 0013): frame number and sub-frame at rate_numerator /
	//! rate_denominator frames per second, the fields of UE's
	//! FQualifiedFrameTime.
	struct SceneTime
	{
		int32_t frame = 0;
		float subframe = 0.0f;
		int32_t rate_numerator = 0;
		int32_t rate_denominator = 0;
	};

	//! Whether a SceneTime can be used: a positive rate, and a finite
	//! sub-frame in [0, 1) (UE's FFrameTime invariant). A writer does not
	//! write an invalid one; a reader treats one as absent and still applies
	//! the frame.
	inline bool IsValidSceneTime(const SceneTime& time)
	{
		return time.rate_numerator > 0 && time.rate_denominator > 0
			&& std::isfinite(time.subframe) && time.subframe >= 0.0f && time.subframe < 1.0f;
	}
}
