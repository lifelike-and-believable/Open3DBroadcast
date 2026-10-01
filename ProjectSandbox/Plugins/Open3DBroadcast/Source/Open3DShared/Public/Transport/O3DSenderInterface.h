// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DTransportTypes.h"

namespace O3DS
{
    class SubjectList;
}

/*
 * The sender side of the transport interface (ADR 0007 items 1 and 3). Moved here from
 * Open3DSender so transports and the Open3DBroadcastWebRTC add-on can depend on Open3DShared
 * alone; the old "O3DSenderInterface.h" forwards here for one release. The class layouts and
 * virtual function tables are unchanged by the move (O3D_TRANSPORT_API_VERSION stays 1).
 * The control channel (ADR 0011) then appended SupportsControl and SendControl (version 2).
 */

/** Interface for audio sinks provided by transports that support PCM ingestion. */
class OPEN3DSHARED_API IO3DSenderAudioSink
{
public:
    virtual ~IO3DSenderAudioSink() = default;

    /** Submit interleaved floating point PCM samples. Returns false if the frame was dropped. */
    virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) = 0;

    /** Notification that the capture path has stopped producing frames. */
    virtual void OnCaptureStopped() {}
};

/** Interface implemented by all transport sender instances. */
class OPEN3DSHARED_API IOpen3DSender
{
public:
    virtual ~IOpen3DSender() = default;

    /** Allocate transport resources and prepare for Start. Non-blocking. */
    virtual bool Initialize(const FO3DTransportConfig& Config) = 0;

    /** Begin async networking / connection establishment. Non-blocking on success. */
    virtual bool Start() = 0;

    /** Stop networking and release resources. Idempotent. */
    virtual void Stop() = 0;

    /** Attempt to send a serialized SubjectList payload. Return false if backpressure drops it.
     *  Implementations call SubjectList::Serialize() (a full topology+value
     *  snapshot) internally - see SendSerialized() below for the path that
     *  transmits whatever encoding the caller already chose instead.
     *  Deprecated (ADR 0007 item 3): nothing on the frame path calls it; WP-A1 PR 5 removes it. */
    virtual bool Send(const O3DS::SubjectList& List) = 0;

    /** Send already-serialized FlatBuffer bytes directly, bypassing this
     *  transport's own SubjectList::Serialize() call in Send() above. Used
     *  by the normal per-frame pose pipeline (FO3DSenderSerializer via
     *  UO3DSenderComponent), which decides once - not per-transport -
     *  whether a frame is a full-sync snapshot or a C2 delta/residual
     *  update (roadmap doc §5/C2) and produces the final wire bytes
     *  itself; every transport should prefer this over re-deriving bytes
     *  from a SubjectList object, since only the caller knows which
     *  encoding was actually used. `SubjectName` mirrors what Send(List)
     *  implementations already extract from List.mItems[0]->mName for
     *  stats/diagnostics - passed explicitly here since raw bytes don't
     *  expose it without a redundant parse. `CaptureTimestampSec` is the
     *  same capture-time value already embedded in `Data` by the caller's
     *  own serialization (FO3DSenderSerializer's `Now`) - a transport that
     *  keeps its own local packet/latency metadata alongside the payload
     *  (e.g. Loopback's queued packet timestamp, MoQ's enqueue-to-publish
     *  latency measurement) should use this rather than sampling a fresh
     *  FPlatformTime::Seconds() itself, or that local metadata drifts from
     *  what's actually encoded on the wire. Default returns false
     *  (unsupported/dropped): a transport that hasn't been updated to
     *  implement this doesn't support the byte-oriented path yet. Note
     *  there is no Send(List) fallback for this failure: the normal frame
     *  pipeline (UO3DSenderComponent::HandleSerializedFrameForward) only
     *  ever has bytes at this point, not a SubjectList, so a false return
     *  here means the frame is dropped for that transport, not resent via
     *  Send(). */
    virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec)
    {
        (void)Data; (void)Len; (void)SubjectName; (void)CaptureTimestampSec;
        return false;
    }

    /** Lightweight upkeep hook. MUST NOT block. */
    virtual void Tick(float DeltaSeconds) = 0;

    /** Snapshot of inline counters. Should be inexpensive to call. */
    virtual FO3DTransportStats GetStats() const = 0;

    /** Whether this sender advertises audio support. Default is false. */
    virtual bool SupportsAudio() const { return false; }

    /** Optional audio sink factory. Default returns nullptr (no audio support). */
    virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) { return nullptr; }

    // Control channel (docs/adr/0011-control-channel.md, item 6). Appended after every existing
    // virtual, so the earlier vtable slots keep their order (O3D_TRANSPORT_API_VERSION 2).

    /** Whether this sender carries control messages. Default false. Any thread. */
    virtual bool SupportsControl() const { return false; }

    /**
     * Send one control envelope (O3DS::WriteControlEnvelope output) to every receiver of this
     * stream. Called on the game thread in v1; implementations must be thread-safe, must not
     * block, and copy the bytes. Not routed through SendSerialized or the pose pipeline, so
     * control is never dropped as an old pose or counted as a mocap frame. Returns false when
     * the sender is not running, does not support control, the bytes are not a control
     * envelope, or the transport's queue is full; the caller (ControlPublisher) retries.
     * Default returns false.
     */
    virtual bool SendControl(const uint8* Envelope, int32 Len)
    {
        (void)Envelope; (void)Len;
        return false;
    }
};

/** Creates one sender instance. Called on the game thread, outside any registry lock. */
using FO3DSenderFactory = TFunction<TSharedPtr<IOpen3DSender, ESPMode::ThreadSafe>()>;
