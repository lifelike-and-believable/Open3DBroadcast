// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

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

    /**
     * A unified envelope header as read (ADR 0009 item 4): magic "O3DU", 24 bytes,
     * little-endian, with a per-stream sequence number. The codec lives in the core
     * (o3ds/wire_format.h, docs/wire-format.md).
     */
    struct OPEN3DSHARED_API FUnifiedHeader
    {
        /** The first 4 bytes as a big-endian number: MagicValueBE() for an envelope. */
        uint32 MagicBE = 0;
        uint8 Version = 1;
        uint8 Kind = 0;
        uint8 Codec = 0;
        uint8 Flags = 0;
        uint64 TimestampUsHost = 0;
        uint32 PayloadSizeHost = 0;
        /** Per stream and kind, wraps. */
        uint32 Seq = 0;
        /** Bytes before the payload (UnifiedWireHeaderSize). */
        int32 HeaderSize = 0;

        /** The envelope magic "O3DU" as a big-endian number. */
        static constexpr uint32 MagicValueBE() { return 0x4F334455u; }
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

    /**
     * Stream label the receiver gives audio whose transport set no label and whose source has no
     * stream id. The remote audio component's Mix mode plays labels that start with it (RCV-21).
     */
    inline constexpr const TCHAR* MixAudioStreamLabel = TEXT("o3ds:mix");

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

    /**
     * Parse a unified envelope without copying: OutPayloadPtr views the payload inside
     * Data. False for anything else, or when the payload does not fit in Size (trailing bytes are
     * allowed).
     */
    OPEN3DSHARED_API bool ParseUnifiedMessage(const uint8* Data, int32 Size,
                                              FUnifiedHeader& OutHeader,
                                              const uint8*& OutPayloadPtr,
                                              int32& OutPayloadSize);

    /** True when Data starts with the envelope magic: never a raw frame, whatever follows. */
    OPEN3DSHARED_API bool HasUnifiedEnvelopeMagic(const uint8* Data, int32 Size);

    /** Size of the envelope header. */
    constexpr int32 UnifiedWireHeaderSize = 24;

    /** Largest payload the unified envelope wraps (safety limit). */
    constexpr int32 UnifiedMaxPayloadSize = 50 * 1024 * 1024;

    /**
     * Write an envelope v2 header into the first UnifiedWireHeaderSize bytes of a message whose
     * payload has already been written after them. The payload size is taken from the buffer, so
     * callers can serialize the payload in place without a second copy (SHR-18). TimestampSec is
     * the sender clock; a negative or non-finite value is written as 0. Seq counts the messages
     * of one stream and kind.
     */
    OPEN3DSHARED_API bool WriteUnifiedHeaderInPlace(EUnifiedKind Kind, EUnifiedCodec Codec, double TimestampSec, TArray<uint8>& InOutMessage, uint32 Seq = 0);

    /**
     * Largest control payload (the ControlMessage inside the envelope). Equal to
     * O3DS::ControlLimits::kMaxPayloadBytes, which O3DControlConvert.cpp checks with a
     * static_assert: Open3DShared's public headers do not include the core. With the header,
     * a control envelope stays within one UDP datagram and the WebRTC lossy limit (ADR 0011
     * item 4), so it is never fragmented.
     */
    constexpr int32 UnifiedMaxControlPayloadSize = 1076;

    /** Create a unified message (envelope v2) by wrapping a payload with the proper header. */
    OPEN3DSHARED_API bool CreateUnifiedMessage(EUnifiedKind Kind, EUnifiedCodec Codec, const uint8* PayloadData, int32 PayloadSize, double TimestampSec, TArray<uint8>& OutMessage, uint32 Seq = 0);

    /**
     * Wrap one control payload (ControlPublisher output) in a control envelope for
     * IOpen3DSender::SendControl. Refuses an empty payload, because TCP receivers read any
     * zero-payload envelope as a keepalive, and a payload over UnifiedMaxControlPayloadSize.
     * TimestampSec is the sender clock (ADR 0009 item 7); a negative or non-finite value is
     * written as 0.
     */
    OPEN3DSHARED_API bool WriteControlEnvelope(TConstArrayView<uint8> Payload, double TimestampSec, TArray<uint8>& OutMessage, uint32 Seq = 0);

    /**
     * The one classifier every receive path uses for control (CTL-3, CTL-6). True when Data is a
     * well-formed control envelope: kind Control, codec O3DControl, and a payload of 1
     * to UnifiedMaxControlPayloadSize bytes. OutPayload then views the bytes inside the envelope,
     * valid as long as Data is; the envelope itself ends at OutPayload's end. A buffer with kind
     * Control that fails any other check is malformed and must be dropped, never treated as mocap.
     */
    OPEN3DSHARED_API bool TryGetControlPayload(const uint8* Data, int32 Size, TConstArrayView<uint8>& OutPayload);
}
