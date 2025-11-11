#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

class ISerializedFrameConsumer;

/** Interface for audio sinks that transports can push PCM16 data into. */
class OPEN3DRECEIVER_API IO3DReceiverAudioSink
{
public:
    virtual ~IO3DReceiverAudioSink() = default;

    /** Submit PCM16 audio payload. Implementations must be thread-safe. */
    virtual void SubmitPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes) = 0;
};

/** Interface implemented by all transport receiver instances. */
class OPEN3DRECEIVER_API IOpen3DReceiver
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

using FO3DReceiverFactory = TFunction<TSharedPtr<IOpen3DReceiver>()>;
