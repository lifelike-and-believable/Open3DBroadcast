// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DUnifiedMessage.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FO3DOnAudioPcm16, const O3DS::FAudioFrameMeta& /*Meta*/, const TArray<uint8>& /*PCM16Bytes*/);

/**
 * Shared audio bus that allows transports to publish decoded PCM16 audio frames which gameplay components can consume.
 * Implemented as a lightweight singleton delegate to avoid coupling transports to specific playback components.
 *
 * Threading (SHR-10): game thread only. The delegate is a plain multicast delegate, which is not thread-safe, so
 * binding, unbinding and broadcasting all happen on the game thread and both functions check it. Publishers on
 * other threads marshal to the game thread first (see FO3DReceiverSource's audio sink).
 */
class OPEN3DSHARED_API FO3DAudioBus
{
public:
    /** Returns the multicast delegate fired whenever a PCM16 frame is published. Game thread only. */
    static FO3DOnAudioPcm16& OnPcm16();

    /** Broadcast a PCM16 payload to all listeners. Game thread only. Returns early when nothing is bound. */
    static void PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes);
};
