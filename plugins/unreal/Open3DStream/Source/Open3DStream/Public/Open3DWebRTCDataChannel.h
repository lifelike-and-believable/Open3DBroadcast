// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include <functional>
#include "IWebRTCConnector.h" // For audio track config and delegate

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

    // ========== Audio (Media Tracks) ==========
    // Create and configure a media audio track for sending Opus via the backend connector.
    // Returns true if the track is ready to accept PushPcm calls.
    bool EnableAudioSend(const IWebRTCConnector::FAudioSendConfig& Config);

    // Push interleaved float PCM to the named stream label (configured via EnableAudioSend).
    // Call in 10–20 ms chunks at the configured sample rate.
    bool PushPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames,
                 int32 NumChannels, int32 SampleRate, double TimestampSec);

    // Access the remote audio callback delegate (called on game thread after Tick).
    IWebRTCConnector::FOnRemoteAudio& OnRemoteAudio();

private:
    class FImpl;
    TUniquePtr<FImpl> Impl;
};
