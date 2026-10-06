// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#include "O3DAudioSerialization.h"

#include "O3DAudioOpus.h"

namespace
{
    constexpr uint8 AudioPayloadVersion = 1;
    constexpr uint8 EncodedAudioPayloadVersion = 2;
    constexpr uint8 PayloadFlagEncoded = 1 << 0;

    template <typename TValue>
    constexpr TValue ClampToUInt16Range(TValue Value)
    {
        return Value > static_cast<TValue>(MAX_uint16) ? static_cast<TValue>(MAX_uint16) : (Value < 0 ? static_cast<TValue>(0) : Value);
    }

    inline void WriteUInt16LE(TArray<uint8>& Buffer, int32& Offset, uint16 Value)
    {
        Buffer[Offset++] = static_cast<uint8>(Value & 0xFF);
        Buffer[Offset++] = static_cast<uint8>((Value >> 8) & 0xFF);
    }

    inline void WriteUInt32LE(TArray<uint8>& Buffer, int32& Offset, uint32 Value)
    {
        Buffer[Offset++] = static_cast<uint8>(Value & 0xFF);
        Buffer[Offset++] = static_cast<uint8>((Value >> 8) & 0xFF);
        Buffer[Offset++] = static_cast<uint8>((Value >> 16) & 0xFF);
        Buffer[Offset++] = static_cast<uint8>((Value >> 24) & 0xFF);
    }

    inline void WriteDoubleLE(TArray<uint8>& Buffer, int32& Offset, double Value)
    {
        static_assert(sizeof(double) == 8, "Unexpected double size");
        const uint8* AsBytes = reinterpret_cast<const uint8*>(&Value);
        for (int32 Index = 0; Index < 8; ++Index)
        {
            Buffer[Offset++] = AsBytes[Index];
        }
    }

    inline void WriteGuidLE(TArray<uint8>& Buffer, int32& Offset, const FGuid& Guid)
    {
        WriteUInt32LE(Buffer, Offset, Guid.A);
        WriteUInt32LE(Buffer, Offset, Guid.B);
        WriteUInt32LE(Buffer, Offset, Guid.C);
        WriteUInt32LE(Buffer, Offset, Guid.D);
    }

    inline uint16 ReadUInt16LE(const uint8* Data)
    {
        return static_cast<uint16>(Data[0]) | (static_cast<uint16>(Data[1]) << 8);
    }

    inline uint32 ReadUInt32LE(const uint8* Data)
    {
        return static_cast<uint32>(Data[0]) |
            (static_cast<uint32>(Data[1]) << 8) |
            (static_cast<uint32>(Data[2]) << 16) |
            (static_cast<uint32>(Data[3]) << 24);
    }

    inline double ReadDoubleLE(const uint8* Data)
    {
        static_assert(sizeof(double) == 8, "Unexpected double size");
        double Value = 0.0;
        uint8* OutBytes = reinterpret_cast<uint8*>(&Value);
        for (int32 Index = 0; Index < 8; ++Index)
        {
            OutBytes[Index] = Data[Index];
        }
        return Value;
    }

    inline FGuid ReadGuidLE(const uint8* Data)
    {
        FGuid Guid;
        Guid.A = ReadUInt32LE(Data + 0);
        Guid.B = ReadUInt32LE(Data + 4);
        Guid.C = ReadUInt32LE(Data + 8);
        Guid.D = ReadUInt32LE(Data + 12);
        return Guid;
    }
}

namespace O3DAudio
{
    namespace
    {
        constexpr int32 Pcm16HeaderSize = 1 /*Version*/ + 1 /*Flags*/ + 2 /*Channels*/ + 4 /*SampleRate*/ + 8 /*Timestamp*/ + 16 /*Guid*/ + 2 /*LabelSize*/ + 2 /*SubjectSize*/ + 4 /*PCMBytes*/;
        constexpr int32 EncodedHeaderSize = 1 /*Version*/ + 1 /*Flags*/ + 1 /*Codec*/ + 1 /*Reserved*/ + 2 /*Channels*/ + 4 /*SampleRate*/ + 8 /*Timestamp*/ + 16 /*Guid*/ + 2 /*LabelSize*/ + 2 /*SubjectSize*/ + 4 /*PayloadBytes*/;

        bool Fail(EAudioParseError* OutError, EAudioParseError Error)
        {
            if (OutError)
            {
                *OutError = Error;
            }
            return false;
        }

        FString ReadUtf8Name(const uint8* Data, int32 NumBytes)
        {
            if (NumBytes <= 0)
            {
                return FString();
            }
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data), NumBytes);
            return FString(Converted.Length(), Converted.Get());
        }

        /**
         * Writes the PCM16 layout (version 1) or the encoded layout (version 2) after PrefixBytes
         * reserved bytes. The two layouts differ only in their first bytes.
         */
        bool SerializeAudioImpl(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes, int32 PrefixBytes, TArray<uint8>& OutBuffer)
        {
            const bool bPcm16Layout = (Codec == O3DS::EUnifiedCodec::PCM16);
            if (!Data || NumBytes <= 0 || PrefixBytes < 0)
            {
                return false;
            }
            if (bPcm16Layout && (NumBytes % static_cast<int32>(sizeof(int16)) != 0))
            {
                return false;
            }

            FTCHARToUTF8 LabelUtf8(*Meta.StreamLabel);
            FTCHARToUTF8 SubjectUtf8(*Meta.SubjectName);

            const int32 LabelLength = LabelUtf8.Length();
            const int32 SubjectLength = SubjectUtf8.Length();
            // The receiver rejects longer names (SHR-8), so do not send them.
            if (LabelLength > MaxNameBytes || SubjectLength > MaxNameBytes)
            {
                return false;
            }

            const uint16 LabelSize = static_cast<uint16>(LabelLength);
            const uint16 SubjectSize = static_cast<uint16>(SubjectLength);

            const int32 HeaderSize = bPcm16Layout ? Pcm16HeaderSize : EncodedHeaderSize;
            const int64 TotalSize64 = static_cast<int64>(PrefixBytes) + HeaderSize + LabelSize + SubjectSize + NumBytes;
            if (TotalSize64 > MAX_int32)
            {
                return false;
            }
            const int32 TotalSize = static_cast<int32>(TotalSize64);
            OutBuffer.SetNumUninitialized(TotalSize);

            int32 Offset = PrefixBytes;
            if (bPcm16Layout)
            {
                OutBuffer[Offset++] = AudioPayloadVersion;
                OutBuffer[Offset++] = 0; // Flags (reserved)
            }
            else
            {
                OutBuffer[Offset++] = EncodedAudioPayloadVersion;
                OutBuffer[Offset++] = PayloadFlagEncoded;
                OutBuffer[Offset++] = static_cast<uint8>(Codec);
                OutBuffer[Offset++] = 0; // Reserved byte for alignment / future use
            }
            WriteUInt16LE(OutBuffer, Offset, static_cast<uint16>(ClampToUInt16Range(Meta.NumChannels)));
            WriteUInt32LE(OutBuffer, Offset, static_cast<uint32>(Meta.SampleRate));
            WriteDoubleLE(OutBuffer, Offset, Meta.TimestampSec);
            WriteGuidLE(OutBuffer, Offset, Meta.SourceGuid);
            WriteUInt16LE(OutBuffer, Offset, LabelSize);
            WriteUInt16LE(OutBuffer, Offset, SubjectSize);
            WriteUInt32LE(OutBuffer, Offset, static_cast<uint32>(NumBytes));

            if (LabelSize > 0)
            {
                FMemory::Memcpy(OutBuffer.GetData() + Offset, LabelUtf8.Get(), LabelSize);
                Offset += LabelSize;
            }

            if (SubjectSize > 0)
            {
                FMemory::Memcpy(OutBuffer.GetData() + Offset, SubjectUtf8.Get(), SubjectSize);
                Offset += SubjectSize;
            }

            FMemory::Memcpy(OutBuffer.GetData() + Offset, Data, NumBytes);
            Offset += NumBytes;

            check(Offset == TotalSize);
            return true;
        }

        /** Parses either layout and range-checks every field before it is used (SHR-8). */
        bool DeserializeAudioImpl(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize, O3DS::FAudioFrameMeta& OutMeta, TArray<uint8>& OutData, EAudioParseError* OutError)
        {
            const bool bPcm16Layout = (Codec == O3DS::EUnifiedCodec::PCM16);
            const int32 HeaderSize = bPcm16Layout ? Pcm16HeaderSize : EncodedHeaderSize;
            if (OutError)
            {
                *OutError = EAudioParseError::None;
            }
            if (!Payload || PayloadSize < HeaderSize)
            {
                return Fail(OutError, EAudioParseError::Truncated);
            }

            int32 Offset = 0;
            const uint8 Version = Payload[Offset++];
            if (bPcm16Layout)
            {
                if (Version != AudioPayloadVersion)
                {
                    return Fail(OutError, EAudioParseError::BadVersion);
                }
                Offset++; // Flags (unused)
            }
            else
            {
                if (Version != EncodedAudioPayloadVersion)
                {
                    return Fail(OutError, EAudioParseError::BadVersion);
                }
                const uint8 Flags = Payload[Offset++];
                const uint8 CodecByte = Payload[Offset++];
                Offset++; // Reserved
                if ((Flags & PayloadFlagEncoded) == 0)
                {
                    return Fail(OutError, EAudioParseError::BadVersion);
                }
                if (CodecByte != static_cast<uint8>(Codec))
                {
                    return Fail(OutError, EAudioParseError::CodecMismatch);
                }
            }

            const uint16 NumChannels = ReadUInt16LE(Payload + Offset);
            Offset += 2;
            const uint32 SampleRate = ReadUInt32LE(Payload + Offset);
            Offset += 4;
            const double TimestampSec = ReadDoubleLE(Payload + Offset);
            Offset += 8;
            const FGuid SourceGuid = ReadGuidLE(Payload + Offset);
            Offset += 16;
            const uint16 LabelSize = ReadUInt16LE(Payload + Offset);
            Offset += 2;
            const uint16 SubjectSize = ReadUInt16LE(Payload + Offset);
            Offset += 2;
            const uint32 DataBytes = ReadUInt32LE(Payload + Offset);
            Offset += 4;

            if (static_cast<int64>(HeaderSize) + LabelSize + SubjectSize + DataBytes > PayloadSize)
            {
                return Fail(OutError, EAudioParseError::Truncated);
            }
            if (LabelSize > MaxNameBytes || SubjectSize > MaxNameBytes)
            {
                return Fail(OutError, EAudioParseError::NameTooLong);
            }
            if (DataBytes == 0 || (bPcm16Layout && (DataBytes % static_cast<uint32>(sizeof(int16)) != 0)))
            {
                return Fail(OutError, EAudioParseError::BadPayloadSize);
            }
            if (SampleRate > static_cast<uint32>(MAX_int32))
            {
                return Fail(OutError, EAudioParseError::BadSampleRate);
            }

            O3DS::FAudioFrameMeta Meta;
            Meta.SourceGuid = SourceGuid;
            Meta.NumChannels = static_cast<int32>(NumChannels);
            Meta.SampleRate = static_cast<int32>(SampleRate);
            Meta.TimestampSec = TimestampSec;
            const EAudioParseError MetaError = ValidateAudioMeta(Codec, Meta);
            if (MetaError != EAudioParseError::None)
            {
                return Fail(OutError, MetaError);
            }

            Meta.StreamLabel = ReadUtf8Name(Payload + Offset, LabelSize);
            Offset += LabelSize;
            Meta.SubjectName = ReadUtf8Name(Payload + Offset, SubjectSize);
            Offset += SubjectSize;

            OutData.Reset();
            OutData.Append(Payload + Offset, static_cast<int32>(DataBytes));
            OutMeta = MoveTemp(Meta);
            return true;
        }
    }

    bool IsSupportedSampleRate(int32 SampleRate)
    {
        switch (SampleRate)
        {
        case 8000:
        case 11025:
        case 12000:
        case 16000:
        case 22050:
        case 24000:
        case 32000:
        case 44100:
        case 48000:
        case 88200:
        case 96000:
            return true;
        default:
            return false;
        }
    }

    EAudioParseError ValidateAudioMeta(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta)
    {
        const bool bOpus = (Codec == O3DS::EUnifiedCodec::Opus);
        if (Meta.NumChannels < 1 || Meta.NumChannels > (bOpus ? 2 : MaxChannels))
        {
            return EAudioParseError::BadChannelCount;
        }
        if (!IsSupportedSampleRate(Meta.SampleRate) || (bOpus && !FO3DAudioOpusEncoder::IsSupportedSampleRate(Meta.SampleRate)))
        {
            return EAudioParseError::BadSampleRate;
        }
        if (!FMath::IsFinite(Meta.TimestampSec))
        {
            return EAudioParseError::BadTimestamp;
        }
        return EAudioParseError::None;
    }

    bool SerializePcm16Frame(const O3DS::FAudioFrameMeta& Meta, const uint8* PCM16Data, int32 NumBytes, TArray<uint8>& OutPayload)
    {
        return SerializeAudioImpl(O3DS::EUnifiedCodec::PCM16, Meta, PCM16Data, NumBytes, 0, OutPayload);
    }

    bool DeserializePcm16Frame(const uint8* Payload, int32 PayloadSize, FPcm16Frame& OutFrame, EAudioParseError* OutError)
    {
        return DeserializeAudioImpl(O3DS::EUnifiedCodec::PCM16, Payload, PayloadSize, OutFrame.Meta, OutFrame.PCM16, OutError);
    }

    bool SerializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* EncodedData, int32 NumBytes, TArray<uint8>& OutPayload)
    {
        return SerializeAudioImpl(Codec, Meta, EncodedData, NumBytes, 0, OutPayload);
    }

    bool SerializeEncodedAudioFrameAfterPrefix(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* EncodedData, int32 NumBytes, int32 PrefixBytes, TArray<uint8>& OutBuffer)
    {
        return SerializeAudioImpl(Codec, Meta, EncodedData, NumBytes, PrefixBytes, OutBuffer);
    }

    bool TryGetAudioPayloadCodec(const uint8* Payload, int32 PayloadSize, O3DS::EUnifiedCodec& OutCodec)
    {
        if (!Payload || PayloadSize <= 0)
        {
            return false;
        }
        if (Payload[0] == AudioPayloadVersion)
        {
            OutCodec = O3DS::EUnifiedCodec::PCM16;
            return true;
        }
        // Encoded layout: version, flags, codec. PCM16 is always written as version 1, so a
        // version 2 payload carries Opus.
        constexpr int32 EncodedCodecOffset = 2;
        if (Payload[0] == EncodedAudioPayloadVersion && PayloadSize > EncodedCodecOffset
            && (Payload[1] & PayloadFlagEncoded) != 0
            && Payload[EncodedCodecOffset] == static_cast<uint8>(O3DS::EUnifiedCodec::Opus))
        {
            OutCodec = O3DS::EUnifiedCodec::Opus;
            return true;
        }
        return false;
    }

    bool DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize, FEncodedAudioFrame& OutFrame, EAudioParseError* OutError)
    {
        if (!DeserializeAudioImpl(Codec, Payload, PayloadSize, OutFrame.Meta, OutFrame.Payload, OutError))
        {
            return false;
        }
        OutFrame.Codec = Codec;
        return true;
    }
}
