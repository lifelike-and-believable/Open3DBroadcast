// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

/*
 * The receiver side of the transport interface (ADR 0007 items 1 and 3). Moved here from
 * Open3DReceiver so transports and the Open3DBroadcastWebRTC add-on can depend on Open3DShared
 * alone; the old "O3DReceiverInterface.h" forwards here for one release. The class layouts and
 * virtual function tables are unchanged by the move (O3D_TRANSPORT_API_VERSION stays 1).
 */

/** Interface for audio sinks that transports can push PCM16 data into. */
class OPEN3DSHARED_API IO3DReceiverAudioSink
{
public:
    virtual ~IO3DReceiverAudioSink() = default;

    /** Submit PCM16 audio payload. Implementations must be thread-safe. */
    virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes) = 0;
};

/** Interface implemented by all transport receiver instances. */
class OPEN3DSHARED_API IOpen3DReceiver
{
public:
    virtual ~IOpen3DReceiver() = default;

    virtual bool Initialize(const FO3DTransportConfig& Config) = 0;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;

    /** Poll available data and deliver to the previously configured sink. Returns processed frames. */
    virtual int32 Poll() = 0;

    virtual FO3DTransportStats GetStats() const = 0;

    /** Whether this receiver advertises audio support. Default implementation returns false. */
    virtual bool SupportsAudio() const { return false; }

    /** Provide an audio sink for transports that support audio. Passing nullptr disables audio delivery. */
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& /*Sink*/, const FO3DTransportAudioConfig& /*AudioConfig*/) {}
};

/** Creates one receiver instance. Called on the game thread, outside any registry lock. */
using FO3DReceiverFactory = TFunction<TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>()>;
