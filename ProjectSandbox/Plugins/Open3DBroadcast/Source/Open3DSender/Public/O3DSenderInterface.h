#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "O3DTransportTypes.h"

namespace O3DS
{
    class SubjectList;
}

/** Interface for audio sinks provided by transports that support PCM ingestion. */
class OPEN3DSENDER_API IO3DSenderAudioSink
{
public:
    virtual ~IO3DSenderAudioSink() = default;

    /** Submit interleaved floating point PCM samples. Returns false if the frame was dropped. */
    virtual bool SubmitPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) = 0;

    /** Notification that the capture path has stopped producing frames. */
    virtual void OnCaptureStopped() {}
};

/** Interface implemented by all transport sender instances. */
class OPEN3DSENDER_API IOpen3DSender
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
     *  transmits whatever encoding the caller already chose instead. */
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
};

using FO3DSenderFactory = TFunction<TSharedPtr<IOpen3DSender>()>;
