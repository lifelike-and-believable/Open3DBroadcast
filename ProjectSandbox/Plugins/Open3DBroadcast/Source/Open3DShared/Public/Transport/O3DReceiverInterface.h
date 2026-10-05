// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

/*
 * The receiver side of the transport interface (ADR 0007 items 1 and 3). Moved here from
 * Open3DReceiver so transports and the Open3DBroadcastWebRTC add-on can depend on Open3DShared
 * alone (WP-A1 step 6 removed the old Open3DReceiver forwarding header). The control channel
 * (ADR 0011) appended SupportsControl and SetControlSink (version 2). WP-A1 PR 3 (version 4)
 * replaced the bool results with FO3DTransportResult (Start without a consumer is NoConsumer) and
 * added capabilities and connection state; SupportsAudio and SupportsControl became non-virtual
 * forwarders to GetCapabilities(), and WP-A1 step 6 removed them: use
 * GetCapabilities().bAudioReceive and GetCapabilities().bControl.
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

namespace O3DTransport
{
	/**
	 * For receivers that carry control in-band (TCP, UDP, NNG): called with a buffer whose envelope
	 * kind is Control. Hands the payload to Sink when the envelope is well-formed
	 * (O3DS::TryGetControlPayload) and drops it otherwise. Never passes the bytes to the frame
	 * consumer. Returns true when the sink received the payload. Inline, so it adds nothing to the
	 * exported interface.
	 */
	inline bool DeliverControlEnvelope(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink, const uint8* Data, int32 Size, const FString& StreamId)
	{
		TConstArrayView<uint8> Payload;
		if (!Sink.IsValid() || !O3DS::TryGetControlPayload(Data, Size, Payload))
		{
			return false;
		}
		Sink->SubmitControl(Payload, StreamId, FPlatformTime::Seconds());
		return true;
	}
}

/** Interface implemented by all transport receiver instances. */
class OPEN3DSHARED_API IOpen3DReceiver
{
public:
    virtual ~IOpen3DReceiver() = default;

    /**
     * Validates Config and allocates what Start needs. Game thread; non-blocking. Leaves the
     * connection state as it is. InvalidConfig when Config is unusable, ResourceUnavailable when
     * a library or subsystem is missing.
     */
    virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) = 0;

    /** The frame consumer, called only from Poll(). Game thread, before Start; nullptr clears it. */
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) = 0;

    /**
     * Begins receiving. Game thread; non-blocking. NotRunning without a successful Initialize,
     * then NoConsumer without a consumer (SetConsumer); both leave the state as it is. Otherwise
     * as IOpen3DSender::Start: the state moves to Failed when it cannot start, and to Connecting
     * or Connected on success.
     */
    virtual FO3DTransportResult Start() = 0;

    /** As IOpen3DSender::Stop: game thread, idempotent, non-blocking; the state moves to Idle. */
    virtual void Stop() = 0;

    /**
     * Delivers what has arrived to the consumer, with bounded work. Game thread; the only place
     * the frame consumer is called. Returns the number of frames delivered.
     */
    virtual int32 Poll() = 0;

    /** Snapshot of the counters and connection state. Any thread; inexpensive. */
    virtual FO3DTransportStats GetStats() const = 0;

    /** As IOpen3DSender::GetCapabilities, for the config this receiver was initialized with. Any thread. */
    virtual FO3DTransportCapabilities GetCapabilities() const = 0;

    /** Current connection state. Any thread; lock-free. */
    virtual EO3DConnectionState GetConnectionState() const = 0;

    /** As IOpen3DSender::SetStateChangedCallback. Game thread, before Start. */
    virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) = 0;

    /**
     * Provide an audio sink, when GetCapabilities().bAudioReceive. Game thread; nullptr disables
     * audio delivery. The sink may be called on any thread.
     */
    virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& /*Sink*/, const FO3DTransportAudioConfig& /*AudioConfig*/) {}

    /**
     * Provide the control sink (docs/adr/0011-control-channel.md, item 6), when
     * GetCapabilities().bControl. Game thread, before Start; nullptr disables control delivery.
     * The receiver holds the sink strongly and releases it in Stop. A receiver delivers only
     * well-formed control envelopes to it and never passes control bytes to the frame consumer.
     */
    virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& /*Sink*/) {}
};

/** Creates one receiver instance. Called on the game thread, outside any registry lock. */
using FO3DReceiverFactory = TFunction<TSharedPtr<IOpen3DReceiver, ESPMode::ThreadSafe>()>;
