// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

// Unified tiny header for multiplexing payloads over a single WebRTC DataChannel
// without impacting existing O3DS flatbuffer messages.
//
// Layout (network byte order / big-endian):
//   uint32 Magic      = 'O3DA' (0x4F334441)
//   uint8  Version    = 1
//   uint8  Kind       = 0 = Mocap (O3DS flatbuffer), 1 = Audio
//   uint8  Codec      = 0 = O3DS (flatbuffer), 1 = Opus, 2 = PCM16
//   uint8  Flags      = bitfield reserved
//   uint64 TimestampUs
//   uint32 PayloadSize (bytes)
//   uint8  Payload[PayloadSize]
//
// Notes:
// - Existing mocap frames remain unchanged (no header). The header is used for
//   audio frames only in this initial integration to avoid breaking receivers.
// - When used, fields are encoded in network byte order. Helpers below perform
//   endian conversion.

namespace O3DS
{
    enum class EUnifiedKind : uint8
    {
        Mocap = 0,
        Audio = 1
    };

    enum class EUnifiedCodec : uint8
    {
        O3DS = 0,   // flatbuffer
        Opus = 1,
        PCM16 = 2
    };

    struct FUnifiedHeader
    {
        uint32 MagicBE = 0;     // 'O3DA'
        uint8  Version = 1;     // 1
        uint8  Kind = 0;        // EUnifiedKind
        uint8  Codec = 0;       // EUnifiedCodec
        uint8  Flags = 0;       // reserved
        uint64 TimestampUsBE = 0; // microseconds since arbitrary epoch
        uint32 PayloadSizeBE = 0; // bytes

        static constexpr uint32 MagicValueBE() { return 0x4F334441u; } // 'O3DA'

        bool IsValidMagic() const { return MagicBE == MagicValueBE(); }
        uint32 PayloadSize() const { return ByteSwap32(PayloadSizeBE); }
        uint64 TimestampUs() const { return ByteSwap64(TimestampUsBE); }
        EUnifiedKind GetKind() const { return static_cast<EUnifiedKind>(Kind); }
        EUnifiedCodec GetCodec() const { return static_cast<EUnifiedCodec>(Codec); }

        static uint16 ByteSwap16(uint16 V)
        {
            return (uint16)((V >> 8) | (V << 8));
        }
        static uint32 ByteSwap32(uint32 V)
        {
            return ((V & 0x000000FFu) << 24) | ((V & 0x0000FF00u) << 8) | ((V & 0x00FF0000u) >> 8) | ((V & 0xFF000000u) >> 24);
        }
        static uint64 ByteSwap64(uint64 V)
        {
            return ((V & 0x00000000000000FFull) << 56) |
                   ((V & 0x000000000000FF00ull) << 40) |
                   ((V & 0x0000000000FF0000ull) << 24) |
                   ((V & 0x00000000FF000000ull) << 8)  |
                   ((V & 0x000000FF00000000ull) >> 8)  |
                   ((V & 0x0000FF0000000000ull) >> 24) |
                   ((V & 0x00FF000000000000ull) >> 40) |
                   ((V & 0xFF00000000000000ull) >> 56);
        }
    };

    // Metadata passed alongside audio payloads (PCM16 in this initial PR)
    struct FAudioFrameMeta
    {
        FString StreamLabel;   // e.g. "o3ds:mix" or "o3ds:subject/<Name>"
        FString SubjectName;   // copied from label if encoded, optional
        int32   NumChannels = 1;
        int32   SampleRate = 48000;
        double  TimestampSec = 0.0; // seconds
    };

    // Parse a unified header from a byte buffer. Returns true on success and
    // sets OutHeader and OutPayloadPtr/OutPayloadSize.
    inline bool ParseUnifiedMessage(const uint8* Data, int32 Size,
                                    FUnifiedHeader& OutHeader,
                                    const uint8*& OutPayloadPtr,
                                    int32& OutPayloadSize)
    {
        const int32 MinSize = sizeof(FUnifiedHeader);
        if (!Data || Size < MinSize)
        {
            return false;
        }
        // Safe memcpy to avoid alignment/packing concerns
        FUnifiedHeader Hdr;
        FMemory::Memcpy(&Hdr, Data, sizeof(FUnifiedHeader));
        if (!Hdr.IsValidMagic())
        {
            return false;
        }
        const uint32 PayloadSize = Hdr.PayloadSize();
        const int64 TotalSize = (int64)sizeof(FUnifiedHeader) + (int64)PayloadSize;
        if (TotalSize > Size)
        {
            return false;
        }
        OutHeader = Hdr;
        OutPayloadPtr = Data + sizeof(FUnifiedHeader);
        OutPayloadSize = (int32)PayloadSize;
        return true;
    }
}
