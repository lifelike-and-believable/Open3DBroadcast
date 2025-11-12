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

    /** Serialize audio metadata and PCM16 payload into a transport-neutral wire buffer (little-endian fields). */
    OPEN3DSHARED_API bool SerializePcm16Frame(const O3DS::FAudioFrameMeta& Meta, const uint8* PCM16Data, int32 NumBytes, TArray<uint8>& OutPayload);

    /** Parse audio metadata and PCM16 payload from a transport-neutral wire buffer (little-endian fields). */
    OPEN3DSHARED_API bool DeserializePcm16Frame(const uint8* Payload, int32 PayloadSize, FPcm16Frame& OutFrame);
}
