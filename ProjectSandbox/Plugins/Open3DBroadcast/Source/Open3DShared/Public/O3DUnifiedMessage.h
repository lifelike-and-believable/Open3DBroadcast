// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

namespace O3DS
{
    /** High-level payload families the unified framing format can transport. */
    enum class EUnifiedKind : uint8
    {
        Mocap = 0,
        Audio = 1,
        /** Control channel (docs/adr/0011-control-channel.md). Old TCP, UDP and NNG receivers
         *  ignore a kind they do not know. Always paired with EUnifiedCodec::O3DControl. */
        Control = 2
    };

    /** Enumerates codecs carried within the unified envelope. */
    enum class EUnifiedCodec : uint8
    {
        O3DS = 0,
        Opus = 1,
        PCM16 = 2,
        /** A ControlMessage FlatBuffer (src/o3ds_control.fbs, identifier "O3DC"). */
        O3DControl = 3
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

    /** Size of the unified envelope header on the wire. */
    constexpr int32 UnifiedWireHeaderSize = 20;

    /** Largest payload the unified envelope wraps (safety limit). */
    constexpr int32 UnifiedMaxPayloadSize = 50 * 1024 * 1024;

    /**
     * Write the unified envelope header into the first UnifiedWireHeaderSize bytes of a message
     * whose payload has already been written after them. The payload size is taken from the
     * buffer, so callers can serialize the payload in place without a second copy (SHR-18).
     */
    inline bool WriteUnifiedHeaderInPlace(EUnifiedKind Kind, EUnifiedCodec Codec, double TimestampSec, TArray<uint8>& InOutMessage)
    {
        const int32 PayloadSize = InOutMessage.Num() - UnifiedWireHeaderSize;
        if (PayloadSize <= 0 || PayloadSize > UnifiedMaxPayloadSize)
        {
            return false;
        }

        // Convert timestamp to microseconds
        const uint64 TimestampUs = static_cast<uint64>(TimestampSec * 1000000.0);

        uint8* WritePtr = InOutMessage.GetData();

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
        return true;
    }

    /**
     * Largest control payload (the ControlMessage inside the envelope). Equal to
     * O3DS::ControlLimits::kMaxPayloadBytes, which O3DControlConvert.cpp checks with a
     * static_assert: Open3DShared's public headers do not include the core. With the header,
     * a control envelope stays within one UDP datagram and the WebRTC lossy limit (ADR 0011
     * item 4), so it is never fragmented.
     */
    constexpr int32 UnifiedMaxControlPayloadSize = 1076;

    /** Create a unified message by wrapping a payload with the proper header. */
    inline bool CreateUnifiedMessage(EUnifiedKind Kind, EUnifiedCodec Codec, const uint8* PayloadData, int32 PayloadSize, double TimestampSec, TArray<uint8>& OutMessage)
    {
        if (!PayloadData || PayloadSize <= 0 || PayloadSize > UnifiedMaxPayloadSize)
        {
            return false;
        }

        OutMessage.SetNumUninitialized(UnifiedWireHeaderSize + PayloadSize);
        FMemory::Memcpy(OutMessage.GetData() + UnifiedWireHeaderSize, PayloadData, PayloadSize);
        return WriteUnifiedHeaderInPlace(Kind, Codec, TimestampSec, OutMessage);
    }

    /**
     * Wrap one control payload (ControlPublisher output) in a control envelope for
     * IOpen3DSender::SendControl. Refuses an empty payload, because TCP receivers read any
     * zero-payload envelope as a keepalive, and a payload over UnifiedMaxControlPayloadSize.
     * TimestampSec is the sender clock (ADR 0009 item 7); a negative or non-finite value is
     * written as 0.
     */
    inline bool WriteControlEnvelope(TConstArrayView<uint8> Payload, double TimestampSec, TArray<uint8>& OutMessage)
    {
        OutMessage.Reset();
        if (Payload.Num() <= 0 || Payload.Num() > UnifiedMaxControlPayloadSize)
        {
            return false;
        }
        const double SafeTimestampSec = (FMath::IsFinite(TimestampSec) && TimestampSec > 0.0) ? TimestampSec : 0.0;
        return CreateUnifiedMessage(EUnifiedKind::Control, EUnifiedCodec::O3DControl, Payload.GetData(), Payload.Num(), SafeTimestampSec, OutMessage);
    }

    /**
     * The one classifier every receive path uses for control (CTL-3, CTL-6). True when Data is a
     * well-formed control envelope: kind Control, codec O3DControl, and a payload of 1 to
     * UnifiedMaxControlPayloadSize bytes. OutPayload then views the bytes inside the envelope,
     * valid as long as Data is. A buffer with kind Control that fails any other check is
     * malformed and must be dropped, never treated as mocap.
     */
    inline bool TryGetControlPayload(const uint8* Data, int32 Size, TConstArrayView<uint8>& OutPayload)
    {
        OutPayload = TConstArrayView<uint8>();
        FUnifiedHeader Header;
        const uint8* PayloadPtr = nullptr;
        int32 PayloadSize = 0;
        if (!ParseUnifiedMessage(Data, Size, Header, PayloadPtr, PayloadSize))
        {
            return false;
        }
        if (Header.GetKind() != EUnifiedKind::Control || Header.GetCodec() != EUnifiedCodec::O3DControl
            || PayloadSize <= 0 || PayloadSize > UnifiedMaxControlPayloadSize)
        {
            return false;
        }
        OutPayload = TConstArrayView<uint8>(PayloadPtr, PayloadSize);
        return true;
    }
}
