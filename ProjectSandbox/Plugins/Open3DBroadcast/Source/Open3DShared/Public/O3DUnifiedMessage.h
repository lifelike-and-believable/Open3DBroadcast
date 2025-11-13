// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

namespace O3DS
{
    /** High-level payload families the unified framing format can transport. */
    enum class EUnifiedKind : uint8
    {
        Mocap = 0,
        Audio = 1
    };

    /** Enumerates codecs carried within the unified envelope. */
    enum class EUnifiedCodec : uint8
    {
        O3DS = 0,
        Opus = 1,
        PCM16 = 2
    };

    /** Wire header shared by all Open3DStream unified messages (big-endian as laid out on the wire). */
    struct OPEN3DSHARED_API FUnifiedHeader
    {
        uint32 MagicBE = 0;
        uint8 Version = 1;
        uint8 Kind = 0;
        uint8 Codec = 0;
        uint8 Flags = 0;
        uint64 TimestampUsHost = 0;
        uint32 PayloadSizeHost = 0;

        static constexpr uint32 MagicValueBE() { return 0x4F334441u; }
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

    /** Metadata accompanying audio frames when surfaced to gameplay systems. */
    struct OPEN3DSHARED_API FAudioFrameMeta
    {
        FGuid SourceGuid;
        FString StreamLabel;
        FString SubjectName;
        int32 NumChannels = 1;
        int32 SampleRate = 48000;
        double TimestampSec = 0.0;
    };

    static void WriteBE32(uint8* Dest, uint32 Value)
    {
        Dest[0] = static_cast<uint8>((Value >> 24) & 0xFF);
        Dest[1] = static_cast<uint8>((Value >> 16) & 0xFF);
        Dest[2] = static_cast<uint8>((Value >> 8) & 0xFF);
        Dest[3] = static_cast<uint8>(Value & 0xFF);
    }

    static void WriteBE64(uint8* Dest, uint64 Value)
    {
        Dest[0] = static_cast<uint8>((Value >> 56) & 0xFF);
        Dest[1] = static_cast<uint8>((Value >> 48) & 0xFF);
        Dest[2] = static_cast<uint8>((Value >> 40) & 0xFF);
        Dest[3] = static_cast<uint8>((Value >> 32) & 0xFF);
        Dest[4] = static_cast<uint8>((Value >> 24) & 0xFF);
        Dest[5] = static_cast<uint8>((Value >> 16) & 0xFF);
        Dest[6] = static_cast<uint8>((Value >> 8) & 0xFF);
        Dest[7] = static_cast<uint8>(Value & 0xFF);
    }

    /** Parse the unified message header/payload without copying, performing sanity checks along the way. */
    inline bool ParseUnifiedMessage(const uint8* Data, int32 Size,
                                    FUnifiedHeader& OutHeader,
                                    const uint8*& OutPayloadPtr,
                                    int32& OutPayloadSize)
    {
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

    /** Create a unified message by wrapping a payload with the proper header. */
    inline bool CreateUnifiedMessage(EUnifiedKind Kind, EUnifiedCodec Codec, const uint8* PayloadData, int32 PayloadSize, double TimestampSec, TArray<uint8>& OutMessage)
    {
        constexpr int32 WireHeaderSize = 20;
        if (!PayloadData || PayloadSize <= 0 || PayloadSize > 50 * 1024 * 1024) // 50 MB safety limit
        {
            return false;
        }

        const int32 TotalSize = WireHeaderSize + PayloadSize;
        OutMessage.SetNumUninitialized(TotalSize);

        // Convert timestamp to microseconds
        const uint64 TimestampUs = static_cast<uint64>(TimestampSec * 1000000.0);

        uint8* WritePtr = OutMessage.GetData();

        // Write magic number (big-endian)
        WriteBE32(WritePtr + 0, FUnifiedHeader::MagicValueBE());

        // Write version, kind, codec, flags
        WritePtr[4] = 1; // Version
        WritePtr[5] = static_cast<uint8>(Kind);
        WritePtr[6] = static_cast<uint8>(Codec);
        WritePtr[7] = 0; // Flags

        // Write timestamp (big-endian)
        WriteBE64(WritePtr + 8, TimestampUs);

        // Write payload size (big-endian)
        WriteBE32(WritePtr + 16, static_cast<uint32>(PayloadSize));

        // Copy payload
        FMemory::Memcpy(WritePtr + WireHeaderSize, PayloadData, PayloadSize);

        return true;
    }
}
