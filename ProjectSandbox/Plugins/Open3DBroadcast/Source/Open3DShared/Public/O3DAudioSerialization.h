// Copyright (c) Open3DStream Contributors

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

    /** Serialize audio metadata and PCM16 payload into a transport-neutral wire buffer (little-endian fields). */
    OPEN3DSHARED_API bool SerializePcm16Frame(const O3DS::FAudioFrameMeta& Meta, const uint8* PCM16Data, int32 NumBytes, TArray<uint8>& OutPayload);

    /** Parse audio metadata and PCM16 payload from a transport-neutral wire buffer (little-endian fields). */
    OPEN3DSHARED_API bool DeserializePcm16Frame(const uint8* Payload, int32 PayloadSize, FPcm16Frame& OutFrame);

    /** Serialize audio metadata and encoded payload for the supplied codec into a transport-neutral wire buffer. */
    OPEN3DSHARED_API bool SerializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const O3DS::FAudioFrameMeta& Meta, const uint8* EncodedData, int32 NumBytes, TArray<uint8>& OutPayload);

    /** Parse audio metadata and encoded payload for the supplied codec from a transport-neutral wire buffer. */
    OPEN3DSHARED_API bool DeserializeEncodedAudioFrame(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize, FEncodedAudioFrame& OutFrame);
}
