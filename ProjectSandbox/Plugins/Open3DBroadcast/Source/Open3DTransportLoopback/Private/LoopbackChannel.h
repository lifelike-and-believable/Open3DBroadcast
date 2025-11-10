#pragma once

#include "CoreMinimal.h"
#include "O3DTransportTypes.h"

#include "Containers/Queue.h"
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

/**
 * Shared channel state storing buffered packets between loopback senders and receivers.
 */
struct FO3DLoopbackChannel
{
    FO3DLoopbackChannel(const FString& InName, int32 InCapacity)
        : Name(InName)
        , Capacity(FMath::Max(1, InCapacity))
        , PendingCount(0)
    {
    }

    FString Name;
    int32 Capacity;
    TQueue<FO3DLoopbackPacket, EQueueMode::Mpsc> Queue;
    std::atomic<int32> PendingCount;
};

namespace O3DLoopback
{
    /** Determine the canonical channel key based on the supplied transport configuration. */
    FString ResolveChannelKey(const FO3DTransportConfig& Config);

    /** Determine the queue capacity using optional advanced parameters. */
    int32 ResolveQueueCapacity(const FO3DTransportConfig& Config);

    /** Retrieve or create the shared loopback channel for the supplied key. */
    TSharedPtr<FO3DLoopbackChannel, ESPMode::ThreadSafe> AcquireChannel(const FString& ChannelKey, int32 Capacity);
}
