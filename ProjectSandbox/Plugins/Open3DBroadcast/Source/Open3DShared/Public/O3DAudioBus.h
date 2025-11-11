// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DUnifiedMessage.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FO3DOnAudioPcm16, const O3DS::FAudioFrameMeta& /*Meta*/, const TArray<uint8>& /*PCM16Bytes*/);

/**
 * Shared audio bus that allows transports to publish decoded PCM16 audio frames which gameplay components can consume.
 * Implemented as a lightweight singleton delegate to avoid coupling transports to specific playback components.
 */
class OPEN3DSHARED_API FO3DAudioBus
{
public:
    /** Returns the multicast delegate fired whenever a PCM16 frame is published. */
    static FO3DOnAudioPcm16& OnPcm16();

    /** Broadcast a PCM16 payload to all listeners. Data is copied to ensure thread safety across publisher threads. */
    static void PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes);
};
