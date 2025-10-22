// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include <functional>

/**
 * Public wrapper for WebRTC DataChannel used by broadcast/receiver modules.
 * Internally delegates to the private FWebRTCConnector implementation in the Open3DStream module.
 */
class OPEN3DSTREAM_API FO3DSWebRTCDataChannel
{
public:
    FO3DSWebRTCDataChannel();
    ~FO3DSWebRTCDataChannel();

    // Start a WebRTC peer connection and open a binary DataChannel.
    // Url example: webrtc://host:port/room?role=client|server&stun=stun:stun.l.google.com:19302
    // If role is omitted, defaults to client.
    bool Start(const FString& Url);

    // Stop the connection and close the DataChannel.
    void Stop();

    // Send a binary message over the DataChannel.
    bool Send(const uint8* Data, int32 Size);

    // Connection/DataChannel state.
    bool IsConnected() const;
    bool IsOpen() const;

    // Set callback invoked on game thread from Tick() for received messages.
    void SetOnMessage(TFunction<void(const uint8*, int32)> InOnMessage);

    // Pump internal queues (call from game thread each tick).
    void Tick();

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
