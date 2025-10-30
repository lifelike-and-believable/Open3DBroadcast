// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include <functional>
#include "IWebRTCConnector.h"

// Forward declarations
enum class EO3DSWebRtcBackendReceiver : uint8;

/**
 * Public wrapper for WebRTC DataChannel used by broadcast/receiver modules.
 * Internally delegates to the IWebRTCConnector interface implementation via factory.
 */
class OPEN3DSTREAM_API FO3DSWebRTCDataChannel
{
public:
    FO3DSWebRTCDataChannel();
    ~FO3DSWebRTCDataChannel();

    // Prepare connector without starting (allows audio configuration before PeerConnection creation).
    // This is optional; Start() will call PrepareConnector() automatically if not already called.
    bool PrepareConnector(EO3DSWebRtcBackendReceiver Backend = static_cast<EO3DSWebRtcBackendReceiver>(0));

    // Start a WebRTC peer connection and open a binary DataChannel.
    // Url example: webrtc://host:port/room?role=client|server&stun=stun.l.google.com:19302
    // If role is omitted, defaults to client.
    // Backend parameter selects between LibDataChannel (P2P) and LiveKit (SFU).
    bool Start(const FString& Url, EO3DSWebRtcBackendReceiver Backend = static_cast<EO3DSWebRtcBackendReceiver>(0));

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

    // Expose underlying connector for advanced use (e.g., audio injection). May be null until Start succeeds.
    TSharedPtr<IWebRTCConnector> GetConnector() const;

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
