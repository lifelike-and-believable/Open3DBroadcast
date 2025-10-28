// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Open3DStreamSourceSettings.h" // EO3DSWebRtcBackendReceiver
#include "IWebRTCConnector.h"

/**
 * Public wrapper for WebRTC DataChannel used by broadcast/receiver modules.
 * Internally delegates to the IWebRTCConnector interface implementation via factory.
 */
class OPEN3DSTREAM_API FO3DSWebRTCDataChannel
{
public:
    FO3DSWebRTCDataChannel();
    ~FO3DSWebRTCDataChannel();

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

    // Pump internal queues (call from game thread each tick).
    void Tick();

    // Message callback for inbound DataChannel messages (if used by receiver)
    void SetOnMessage(TFunction<void(const uint8*, int32)> InOnMessage);

    // NEW: expose active backend-agnostic connector (so other systems can push audio)
    TSharedPtr<IWebRTCConnector> GetConnector() const;

    // NEW: forward audio APIs to the underlying connector
    bool EnableAudioSend(const IWebRTCConnector::FAudioSendConfig& Config);
    bool PushPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
