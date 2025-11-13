// Copyright (c) Open3DStream Contributors

#include "O3DAudioSerialization.h"

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
    bool SerializePcm16Frame(const O3DS::FAudioFrameMeta& Meta, const uint8* PCM16Data, int32 NumBytes, TArray<uint8>& OutPayload)
    {
        if (PCM16Data == nullptr || NumBytes <= 0 || (NumBytes % static_cast<int32>(sizeof(int16)) != 0))
        {
            return false;
        }

        FTCHARToUTF8 LabelUtf8(*Meta.StreamLabel);
        FTCHARToUTF8 SubjectUtf8(*Meta.SubjectName);

        const int32 LabelLength = LabelUtf8.Length();
        const int32 SubjectLength = SubjectUtf8.Length();
        if (LabelLength > MAX_uint16 || SubjectLength > MAX_uint16)
        {
            return false;
        }

        const uint16 LabelSize = static_cast<uint16>(LabelLength);
        const uint16 SubjectSize = static_cast<uint16>(SubjectLength);

        constexpr int32 HeaderSize = 1 /*Version*/ + 1 /*Flags*/ + 2 /*Channels*/ + 4 /*SampleRate*/ + 8 /*Timestamp*/ + 16 /*Guid*/ + 2 /*LabelSize*/ + 2 /*SubjectSize*/ + 4 /*PCMBytes*/;
        const int32 TotalSize = HeaderSize + LabelSize + SubjectSize + NumBytes;
        OutPayload.SetNumUninitialized(TotalSize);

        int32 Offset = 0;
        OutPayload[Offset++] = AudioPayloadVersion;
        OutPayload[Offset++] = 0; // Flags (reserved)
        WriteUInt16LE(OutPayload, Offset, static_cast<uint16>(ClampToUInt16Range(Meta.NumChannels)));
        WriteUInt32LE(OutPayload, Offset, static_cast<uint32>(Meta.SampleRate));
        WriteDoubleLE(OutPayload, Offset, Meta.TimestampSec);
        WriteGuidLE(OutPayload, Offset, Meta.SourceGuid);
        WriteUInt16LE(OutPayload, Offset, LabelSize);
        WriteUInt16LE(OutPayload, Offset, SubjectSize);
        WriteUInt32LE(OutPayload, Offset, static_cast<uint32>(NumBytes));

        if (LabelSize > 0)
        {
            FMemory::Memcpy(OutPayload.GetData() + Offset, LabelUtf8.Get(), LabelSize);
            Offset += LabelSize;
        }

        if (SubjectSize > 0)
        {
            FMemory::Memcpy(OutPayload.GetData() + Offset, SubjectUtf8.Get(), SubjectSize);
            Offset += SubjectSize;
        }

        FMemory::Memcpy(OutPayload.GetData() + Offset, PCM16Data, NumBytes);
        Offset += NumBytes;

        check(Offset == TotalSize);
        return true;
    }

    bool DeserializePcm16Frame(const uint8* Payload, int32 PayloadSize, FPcm16Frame& OutFrame)
    {
        if (!Payload || PayloadSize <= 0)
        {
            return false;
        }

        constexpr int32 HeaderSize = 1 + 1 + 2 + 4 + 8 + 16 + 2 + 2 + 4;
        if (PayloadSize < HeaderSize)
        {
            return false;
        }

        int32 Offset = 0;
        const uint8 Version = Payload[Offset++];
        if (Version != AudioPayloadVersion)
        {
            return false;
        }

        Offset++; // Flags (unused)

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
        const uint32 PCMByteCount = ReadUInt32LE(Payload + Offset);
        Offset += 4;

        if (static_cast<int64>(HeaderSize) + LabelSize + SubjectSize + PCMByteCount > PayloadSize)
        {
            return false;
        }

        FString StreamLabel;
        if (LabelSize > 0)
        {
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Payload + Offset), LabelSize);
            StreamLabel = FString(Converted.Length(), Converted.Get());
            Offset += LabelSize;
        }

        FString SubjectName;
        if (SubjectSize > 0)
        {
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Payload + Offset), SubjectSize);
            SubjectName = FString(Converted.Length(), Converted.Get());
            Offset += SubjectSize;
        }

        if (PCMByteCount == 0 || (PCMByteCount % static_cast<uint32>(sizeof(int16)) != 0))
        {
            return false;
        }

        const uint8* PCMPtr = Payload + Offset;
        OutFrame.PCM16.Reset();
        OutFrame.PCM16.AddUninitialized(static_cast<int32>(PCMByteCount));
        FMemory::Memcpy(OutFrame.PCM16.GetData(), PCMPtr, PCMByteCount);

        OutFrame.Meta.SourceGuid = SourceGuid;
        OutFrame.Meta.StreamLabel = MoveTemp(StreamLabel);
        OutFrame.Meta.SubjectName = MoveTemp(SubjectName);
        OutFrame.Meta.NumChannels = static_cast<int32>(NumChannels);
        OutFrame.Meta.SampleRate = static_cast<int32>(SampleRate);
        OutFrame.Meta.TimestampSec = TimestampSec;

        return true;
    }

    bool SerializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* EncodedData, int32 NumBytes, TArray<uint8>& OutPayload)
    {
        if (!EncodedData || NumBytes <= 0)
        {
            return false;
        }

        if (Codec == O3DS::EUnifiedCodec::PCM16)
        {
            return SerializePcm16Frame(Meta, EncodedData, NumBytes, OutPayload);
        }

        FTCHARToUTF8 LabelUtf8(*Meta.StreamLabel);
        FTCHARToUTF8 SubjectUtf8(*Meta.SubjectName);

        const int32 LabelLength = LabelUtf8.Length();
        const int32 SubjectLength = SubjectUtf8.Length();
        if (LabelLength > MAX_uint16 || SubjectLength > MAX_uint16)
        {
            return false;
        }

        const uint16 LabelSize = static_cast<uint16>(LabelLength);
        const uint16 SubjectSize = static_cast<uint16>(SubjectLength);

        constexpr int32 HeaderSize = 1 /*Version*/ + 1 /*Flags*/ + 1 /*Codec*/ + 1 /*Reserved*/ + 2 /*Channels*/ + 4 /*SampleRate*/ + 8 /*Timestamp*/ + 16 /*Guid*/ + 2 /*LabelSize*/ + 2 /*SubjectSize*/ + 4 /*PayloadBytes*/;
        const int32 TotalSize = HeaderSize + LabelSize + SubjectSize + NumBytes;
        OutPayload.SetNumUninitialized(TotalSize);

        int32 Offset = 0;
        OutPayload[Offset++] = EncodedAudioPayloadVersion;
        OutPayload[Offset++] = PayloadFlagEncoded;
        OutPayload[Offset++] = static_cast<uint8>(Codec);
        OutPayload[Offset++] = 0; // Reserved byte for alignment / future use
        WriteUInt16LE(OutPayload, Offset, static_cast<uint16>(ClampToUInt16Range(Meta.NumChannels)));
        WriteUInt32LE(OutPayload, Offset, static_cast<uint32>(Meta.SampleRate));
        WriteDoubleLE(OutPayload, Offset, Meta.TimestampSec);
        WriteGuidLE(OutPayload, Offset, Meta.SourceGuid);
        WriteUInt16LE(OutPayload, Offset, LabelSize);
        WriteUInt16LE(OutPayload, Offset, SubjectSize);
        WriteUInt32LE(OutPayload, Offset, static_cast<uint32>(NumBytes));

        if (LabelSize > 0)
        {
            FMemory::Memcpy(OutPayload.GetData() + Offset, LabelUtf8.Get(), LabelSize);
            Offset += LabelSize;
        }

        if (SubjectSize > 0)
        {
            FMemory::Memcpy(OutPayload.GetData() + Offset, SubjectUtf8.Get(), SubjectSize);
            Offset += SubjectSize;
        }

        FMemory::Memcpy(OutPayload.GetData() + Offset, EncodedData, NumBytes);
        Offset += NumBytes;

        check(Offset == TotalSize);
        return true;
    }

    bool DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize, FEncodedAudioFrame& OutFrame)
    {
        if (!Payload || PayloadSize <= 0)
        {
            return false;
        }

        if (Codec == O3DS::EUnifiedCodec::PCM16)
        {
            FPcm16Frame PcmFrame;
            if (!DeserializePcm16Frame(Payload, PayloadSize, PcmFrame))
            {
                return false;
            }

            OutFrame.Codec = Codec;
            OutFrame.Meta = MoveTemp(PcmFrame.Meta);
            OutFrame.Payload = MoveTemp(PcmFrame.PCM16);
            return true;
        }

        constexpr int32 HeaderSize = 1 + 1 + 1 + 1 + 2 + 4 + 8 + 16 + 2 + 2 + 4;
        if (PayloadSize < HeaderSize)
        {
            return false;
        }

        int32 Offset = 0;
        const uint8 Version = Payload[Offset++];
        if (Version != EncodedAudioPayloadVersion)
        {
            return false;
        }

        const uint8 Flags = Payload[Offset++];
        const uint8 CodecByte = Payload[Offset++];
        Offset++; // Reserved

        if ((Flags & PayloadFlagEncoded) == 0)
        {
            return false;
        }

        if (CodecByte != static_cast<uint8>(Codec))
        {
            return false;
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
        const uint32 PayloadBytes = ReadUInt32LE(Payload + Offset);
        Offset += 4;

        if (static_cast<int64>(HeaderSize) + LabelSize + SubjectSize + PayloadBytes > PayloadSize)
        {
            return false;
        }

        FString StreamLabel;
        if (LabelSize > 0)
        {
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Payload + Offset), LabelSize);
            StreamLabel = FString(Converted.Length(), Converted.Get());
            Offset += LabelSize;
        }

        FString SubjectName;
        if (SubjectSize > 0)
        {
            FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Payload + Offset), SubjectSize);
            SubjectName = FString(Converted.Length(), Converted.Get());
            Offset += SubjectSize;
        }

        if (PayloadBytes == 0)
        {
            return false;
        }

        OutFrame.Payload.Reset();
        OutFrame.Payload.AddUninitialized(static_cast<int32>(PayloadBytes));
        FMemory::Memcpy(OutFrame.Payload.GetData(), Payload + Offset, PayloadBytes);

        OutFrame.Codec = Codec;
        OutFrame.Meta.SourceGuid = SourceGuid;
        OutFrame.Meta.StreamLabel = MoveTemp(StreamLabel);
        OutFrame.Meta.SubjectName = MoveTemp(SubjectName);
        OutFrame.Meta.NumChannels = static_cast<int32>(NumChannels);
        OutFrame.Meta.SampleRate = static_cast<int32>(SampleRate);
        OutFrame.Meta.TimestampSec = TimestampSec;

        return true;
    }
}
