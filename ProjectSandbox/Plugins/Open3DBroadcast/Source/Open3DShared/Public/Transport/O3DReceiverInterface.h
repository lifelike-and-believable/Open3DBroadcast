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
 * The control channel (ADR 0011) then appended SupportsControl and SetControlSink (version 2).
 */

/** Interface for audio sinks that transports can push PCM16 data into. */
class OPEN3DSHARED_API IO3DReceiverAudioSink
{
public:
    virtual ~IO3DReceiverAudioSink() = default;

    /** Submit PCM16 audio payload. Implementations must be thread-safe. */
    virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes) = 0;
};

/**
 * Receives control payloads from a transport (docs/adr/0011-control-channel.md, item 6).
 * SubmitControl may be called on any thread (a socket, worker or FFI thread, or the thread that
 * calls Poll), so implementations must be thread-safe and must not call back into the transport.
 */
class OPEN3DSHARED_API IO3DReceiverControlSink
{
public:
    virtual ~IO3DReceiverControlSink() = default;

    /**
     * One control payload: the ControlMessage bytes inside a control envelope, already checked
     * with O3DS::TryGetControlPayload. The view is valid only for the call; copy to keep it.
     * StreamId is the receiving transport's stream; ReceiveTimeSec is FPlatformTime::Seconds()
     * on arrival.
     */
    virtual void SubmitControl(TConstArrayView<uint8> Payload, const FString& StreamId, double ReceiveTimeSec) = 0;
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

    // Control channel (docs/adr/0011-control-channel.md, item 6). Appended after every existing
    // virtual, so the earlier vtable slots keep their order (O3D_TRANSPORT_API_VERSION 2).

    /** Whether this receiver delivers control payloads. Default false. Any thread. */
    virtual bool SupportsControl() const { return false; }

    /**
     * Provide the control sink. Game thread, before Start; nullptr disables control delivery.
     * The receiver holds the sink strongly and releases it in Stop. A receiver delivers only
     * well-formed control envelopes to it and never passes control bytes to the frame consumer.
     */
    virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& /*Sink*/) {}
};

/** Creates one receiver instance. Called on the game thread, outside any registry lock. */
using FO3DReceiverFactory = TFunction<TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>()>;
