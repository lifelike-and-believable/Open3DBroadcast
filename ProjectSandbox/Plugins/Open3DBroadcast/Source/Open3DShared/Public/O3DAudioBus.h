// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Delegates/Delegate.h"
#include "O3DUnifiedMessage.h"

/** PCM16Bytes is a view of the publisher's buffer, valid only for the duration of the broadcast (SHR-18). */
DECLARE_MULTICAST_DELEGATE_TwoParams(FO3DOnAudioPcm16, const O3DS::FAudioFrameMeta& /*Meta*/, TConstArrayView<uint8> /*PCM16Bytes*/);

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

    /**
     * Broadcast a PCM16 payload to all listeners without copying it (SHR-18). Game thread only. Returns early when
     * nothing is bound. Listeners that keep the bytes must copy them.
     */
    static void PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes);
};
