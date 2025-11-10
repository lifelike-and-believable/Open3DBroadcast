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

    /** Attempt to send a serialized SubjectList payload. Return false if backpressure drops it. */
    virtual bool Send(const O3DS::SubjectList& List) = 0;

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
