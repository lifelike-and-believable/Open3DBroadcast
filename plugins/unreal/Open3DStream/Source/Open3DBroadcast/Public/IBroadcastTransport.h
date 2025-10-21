// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

// Lightweight transport interface for broadcasting serialized O3DS buffers
class IBroadcastTransport
{
public:
    virtual ~IBroadcastTransport() = default;

    // Initialize/connect using provided settings
    virtual bool Start(const FString& InUrl, const FString& InProtocol, const FString& InKey) = 0;
    // Stop/teardown; safe to call multiple times
    virtual void Stop() = 0;

    // Non-blocking where possible; may block briefly if underlying API requires
    virtual bool Send(const uint8* Data, int32 Size, double TimestampSeconds) = 0;

    virtual bool IsConnected() const = 0;

    struct FCounters
    {
        TAtomic<uint64> FramesSent{0};
        TAtomic<uint64> BytesSent{0};
        TAtomic<uint64> FramesDropped{0};
        TAtomic<uint64> Reconnects{0};
    };

    virtual const FCounters& GetCounters() const = 0;
};
