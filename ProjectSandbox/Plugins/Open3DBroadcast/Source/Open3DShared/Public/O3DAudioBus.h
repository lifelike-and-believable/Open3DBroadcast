// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Delegates/Delegate.h"
#include "O3DUnifiedMessage.h"

/** PCM16Bytes is a view of the publisher's buffer, valid only for the duration of the broadcast (SHR-18). */
DECLARE_MULTICAST_DELEGATE_TwoParams(FO3DOnAudioPcm16, const O3DS::FAudioFrameMeta& /*Meta*/, TConstArrayView<uint8> /*PCM16Bytes*/);

/**
 * Audio bus that lets receiver sources publish decoded PCM16 audio frames which gameplay components consume, without
 * coupling transports to specific playback components.
 *
 * Each FO3DRuntimeContext owns one bus (FInstance, docs/adr/0012-runtime-services-and-global-state.md). The statics
 * below use the default context's bus (FO3DRuntimeContext::Default()), so code written against them behaves as before.
 *
 * Threading (SHR-10): game thread only. The delegate is a plain multicast delegate, which is not thread-safe, so
 * binding, unbinding and broadcasting all happen on the game thread and every function checks it. Publishers on
 * other threads marshal to the game thread first (see FO3DReceiverSource's audio sink).
 */
class OPEN3DSHARED_API FO3DAudioBus
{
public:
    /** One audio bus. Obtained from FO3DRuntimeContext::GetAudioBus(); not copyable. */
    class OPEN3DSHARED_API FInstance
    {
    public:
        FInstance() = default;
        FInstance(const FInstance&) = delete;
        FInstance& operator=(const FInstance&) = delete;

        /** Returns the multicast delegate fired whenever a PCM16 frame is published on this bus. Game thread only. */
        FO3DOnAudioPcm16& OnPcm16();

        /**
         * Broadcast a PCM16 payload to this bus's listeners without copying it (SHR-18). Game thread only. Returns
         * early when nothing is bound. Listeners that keep the bytes must copy them.
         */
        void PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes);

    private:
        FO3DOnAudioPcm16 Delegate;
    };

    /** The default context's OnPcm16(). Game thread only. */
    static FO3DOnAudioPcm16& OnPcm16();

    /** The default context's PublishPcm16(). Game thread only. */
    static void PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes);
};
