#pragma once

#include "CoreMinimal.h"
#include "O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Templates/SharedPointer.h"

#include <atomic>

DECLARE_LOG_CATEGORY_EXTERN(LogO3DLoopbackTransport, Log, All);

/**
 * Internal data packet stored within the loopback channel queue.
 */
struct FO3DLoopbackPacket
{
    FString Subject;
    TArray<uint8> Payload;
    double TimestampSeconds = 0.0;
};

struct FO3DLoopbackAudioPacket
{
    O3DS::EUnifiedCodec Codec = O3DS::EUnifiedCodec::PCM16;
    O3DS::FAudioFrameMeta Meta;
    TArray<uint8> Payload;
    double TimestampSeconds = 0.0;
};

/**
 * Shared channel state storing buffered packets between loopback senders and receivers.
 */
struct FO3DLoopbackChannel
{
    FO3DLoopbackChannel(const FString& InName, int32 InCapacity, int32 InAudioCapacity)
        : Name(InName)
        , Capacity(FMath::Max(1, InCapacity))
        , AudioCapacity(FMath::Max(1, InAudioCapacity))
        , PendingCount(0)
        , AudioPendingCount(0)
    {
    }

    FString Name;
    int32 Capacity;
    int32 AudioCapacity;
    TQueue<FO3DLoopbackPacket, EQueueMode::Mpsc> Queue;
    TQueue<FO3DLoopbackAudioPacket, EQueueMode::Mpsc> AudioQueue;
    std::atomic<int32> PendingCount;
    std::atomic<int32> AudioPendingCount;

    void SetLastSubjectName(const FString& InSubject)
    {
        FScopeLock Lock(&MetadataMutex);
        LastSubjectName = InSubject;
    }

    FString GetLastSubjectName() const
    {
        FScopeLock Lock(&MetadataMutex);
        return LastSubjectName;
    }

private:
    mutable FCriticalSection MetadataMutex;
    FString LastSubjectName;
};

namespace O3DLoopback
{
    /** Determine the canonical channel key based on the supplied transport configuration. */
    FString ResolveChannelKey(const FO3DTransportConfig& Config);

    /** Determine the queue capacity using optional advanced parameters. */
    int32 ResolveQueueCapacity(const FO3DTransportConfig& Config);

    /** Determine the audio queue capacity using optional advanced parameters. */
    int32 ResolveAudioQueueCapacity(const FO3DTransportConfig& Config);

    /** Retrieve or create the shared loopback channel for the supplied key. */
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> AcquireChannel(const FString& ChannelKey, int32 Capacity, int32 AudioCapacity);

    /** Returns the current debug level for loopback transport instrumentation. */
    int32 GetAudioDebugLevel();
}
