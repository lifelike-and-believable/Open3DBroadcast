// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DUnifiedMessage.h"

namespace O3DAudio
{
    /** Encapsulated PCM16 audio payload with associated metadata. */
    struct FPcm16Frame
    {
        O3DS::FAudioFrameMeta Meta;
        TArray<uint8> PCM16;
    };

    /** Generic encoded audio payload with metadata and codec marker. */
    struct FEncodedAudioFrame
    {
        O3DS::EUnifiedCodec Codec = O3DS::EUnifiedCodec::PCM16;
        O3DS::FAudioFrameMeta Meta;
        TArray<uint8> Payload;
    };

    /**
     * Why a wire buffer was rejected (SHR-8). Lets receivers count rejects by kind instead of
     * logging each packet from an untrusted peer.
     */
    enum class EAudioParseError : uint8
    {
        None = 0,
        /** Null buffer, or a buffer shorter than its header or declared lengths. */
        Truncated,
        /** Unknown payload version or flags. */
        BadVersion,
        /** The codec byte does not match the codec the caller asked for. */
        CodecMismatch,
        /** Channel count outside [1, MaxChannels] (or above 2 for Opus). */
        BadChannelCount,
        /** Sample rate not in the supported set (or not an Opus rate for Opus). */
        BadSampleRate,
        /** Timestamp is NaN or infinite. */
        BadTimestamp,
        /** Stream label or subject longer than MaxNameBytes. */
        NameTooLong,
        /** Empty, or PCM16 with an odd byte count. */
        BadPayloadSize,
    };

    /** Largest channel count accepted on the wire. */
    constexpr int32 MaxChannels = 8;

    /** Largest stream label or subject name accepted on the wire, in UTF-8 bytes. */
    constexpr int32 MaxNameBytes = 256;

    /** Sample rates accepted on the wire: 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000. */
    OPEN3DSHARED_API bool IsSupportedSampleRate(int32 SampleRate);

    /**
     * Range-check metadata for the given codec (SHR-8): channel count, sample rate and a finite
     * timestamp. Name lengths are checked on the wire bytes by the parsers.
     */
    OPEN3DSHARED_API EAudioParseError ValidateAudioMeta(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta);

    /** Serialize audio metadata and PCM16 payload into a transport-neutral wire buffer (little-endian fields). */
    OPEN3DSHARED_API bool SerializePcm16Frame(const O3DS::FAudioFrameMeta& Meta, const uint8* PCM16Data, int32 NumBytes, TArray<uint8>& OutPayload);

    /**
     * Parse audio metadata and PCM16 payload from a transport-neutral wire buffer (little-endian
     * fields). Metadata is range-checked (SHR-8); OutError, when given, says why a buffer was
     * rejected.
     */
    OPEN3DSHARED_API bool DeserializePcm16Frame(const uint8* Payload, int32 PayloadSize, FPcm16Frame& OutFrame, EAudioParseError* OutError = nullptr);

    /** Serialize audio metadata and encoded payload for the supplied codec into a transport-neutral wire buffer. */
    OPEN3DSHARED_API bool SerializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* EncodedData, int32 NumBytes, TArray<uint8>& OutPayload);

    /**
     * Same as SerializeEncodedAudioFrame, but leaves PrefixBytes uninitialised bytes at the start
     * of OutBuffer for an envelope header, so the envelope needs no second copy (SHR-18).
     */
    OPEN3DSHARED_API bool SerializeEncodedAudioFrameAfterPrefix(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* EncodedData, int32 NumBytes, int32 PrefixBytes, TArray<uint8>& OutBuffer);

    /**
     * The codec of a serialized audio payload (no envelope), read from its header: a version 1
     * payload is PCM16, a version 2 payload names its codec (Opus). For channels that carry no
     * envelope, such as MoQ's audio track (TRF-37). Only peeks; DeserializeEncodedAudioFrame
     * still validates the whole payload. False for anything else.
     */
    OPEN3DSHARED_API bool TryGetAudioPayloadCodec(const uint8* Payload, int32 PayloadSize, O3DS::EUnifiedCodec& OutCodec);

    /**
     * Parse audio metadata and encoded payload for the supplied codec from a transport-neutral
     * wire buffer. Metadata is range-checked (SHR-8); OutError, when given, says why a buffer
     * was rejected.
     */
    OPEN3DSHARED_API bool DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize, FEncodedAudioFrame& OutFrame, EAudioParseError* OutError = nullptr);
}
