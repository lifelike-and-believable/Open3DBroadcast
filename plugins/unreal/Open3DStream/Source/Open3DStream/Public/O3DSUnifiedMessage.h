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
        // Raw fields decoded to host order by ParseUnifiedMessage
        uint32 MagicBE = 0;        // 'O3DA' as big-endian literal for validation
        uint8  Version = 1;        // 1
        uint8  Kind = 0;           // EUnifiedKind
        uint8  Codec = 0;          // EUnifiedCodec
        uint8  Flags = 0;          // reserved
        uint64 TimestampUsHost = 0; // microseconds since epoch, host order
        uint32 PayloadSizeHost = 0; // bytes, host order

        static constexpr uint32 MagicValueBE() { return 0x4F334441u; } // 'O3DA'

        bool IsValidMagic() const { return MagicBE == MagicValueBE(); }
        uint32 PayloadSize() const { return PayloadSizeHost; }
        uint64 TimestampUs() const { return TimestampUsHost; }
        EUnifiedKind GetKind() const { return static_cast<EUnifiedKind>(Kind); }
        EUnifiedCodec GetCodec() const { return static_cast<EUnifiedCodec>(Codec); }

        static uint32 ReadBE32(const uint8* P)
        {
            return (uint32(P[0]) << 24) | (uint32(P[1]) << 16) | (uint32(P[2]) << 8) | uint32(P[3]);
        }
        static uint64 ReadBE64(const uint8* P)
        {
            return (uint64(P[0]) << 56) | (uint64(P[1]) << 48) | (uint64(P[2]) << 40) | (uint64(P[3]) << 32) |
                   (uint64(P[4]) << 24) | (uint64(P[5]) << 16) | (uint64(P[6]) << 8)  | uint64(P[7]);
        }
    };

    // Metadata passed alongside audio payloads (PCM16 in this initial PR)
    struct FAudioFrameMeta
    {
        // Optional identifier of the receiver source emitting this audio (future multi-source)
        FGuid SourceGuid;         // default invalid for single-source setups

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
        // Header is exactly 20 bytes on the wire: 4 + 1 + 1 + 1 + 1 + 8 + 4
        constexpr int32 WireHeaderSize = 20;
        if (!Data || Size < WireHeaderSize)
        {
            return false;
        }

        FUnifiedHeader H;
        H.MagicBE = FUnifiedHeader::ReadBE32(Data + 0);
        if (!H.IsValidMagic())
        {
            return false;
        }
        H.Version = Data[4];
        H.Kind = Data[5];
        H.Codec = Data[6];
        H.Flags = Data[7];
        H.TimestampUsHost = FUnifiedHeader::ReadBE64(Data + 8);
        H.PayloadSizeHost = FUnifiedHeader::ReadBE32(Data + 16);

        const int64 Total = (int64)WireHeaderSize + (int64)H.PayloadSizeHost;
        if (Total > Size)
        {
            return false;
        }
        OutHeader = H;
        OutPayloadPtr = Data + WireHeaderSize;
        OutPayloadSize = (int32)H.PayloadSizeHost;
        return true;
    }
}
