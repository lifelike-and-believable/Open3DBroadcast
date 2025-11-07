// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IWebRTCConnector.h"
#include "Templates/SharedPointer.h"

// Forward declarations
class FO3DSOpusDecoder;
struct FOpen3DStreamSettings;

/**
 * WebRTC receiver adapter for Open3DStream Live Link source.
 * Owns an IWebRTCConnector in server role, binds to DataChannel and audio RTP callbacks,
 * and forwards data to the parsing pipeline and audio decoder.
 * 
 * All I/O and decoding occur off the game thread; callbacks are marshaled to the game thread.
 * 
 * Ground truth: WebRTCConnectorComponent is authoritative for connector behavior.
 */
class FOpen3DSWebRtcReceiver : public TSharedFromThis<FOpen3DSWebRtcReceiver>
{
public:
    FOpen3DSWebRtcReceiver();
    ~FOpen3DSWebRtcReceiver();

    /**
     * Start the receiver: create connector in server role, configure signaling/room, bind callbacks.
     * @param Settings - Live Link source settings (URL, room, audio flags)
     * @return true if connector started successfully
     */
    bool Start(const FOpen3DStreamSettings& Settings);

    /**
     * Stop the receiver: tear down connector and decoder, release resources.
     */
    void Stop();

    /**
     * Tick: pump connector events (non-blocking).
     * Called from FOpen3DStreamSource::Tick on the game thread.
     */
    void Tick(float DeltaSeconds);

    /**
     * @return true if connector is open and ready to receive
     */
    bool IsOpen() const;

    /**
     * Bind a callback to receive DataChannel bytes (for O3DS mocap parsing).
     * @param Callback - delegate to invoke with raw O3DS buffer
     */
    void SetOnDataCallback(TFunction<void(const TArray<uint8>&)> Callback);

    /**
     * Bind a callback to receive state changes (connected, disconnected, errors).
     * @param Callback - delegate to invoke with state message and error flag
     */
    void SetOnStateCallback(TFunction<void(const FString&, bool)> Callback);

private:
    // Connector instance (server role)
    TSharedPtr<IWebRTCConnector> Connector;

    // Opus decoder (RTP → PCM16)
    TSharedPtr<FO3DSOpusDecoder> OpusDecoder;

    // Callbacks from source
    TFunction<void(const TArray<uint8>&)> OnDataCallback;
    TFunction<void(const FString&, bool)> OnStateCallback;

    // Internal connector event handlers (bound to connector delegates)
    void OnConnectorState(const FString& State, bool bIsError);
    void OnConnectorData(const TArray<uint8>& Bytes);
    void OnConnectorAudioRtp(const TArray<uint8>& RtpBytes);

    // State tracking
    bool bStarted = false;
    bool bAudioEnabled = false;
    bool bPreferPcmCallback = false; // when true and PCM callback available, do not decode RTP

    // Data coalescing: keep only latest payload, schedule single GT dispatch
    FCriticalSection CoalesceMutex;
    TArray<uint8> PendingData;
    TAtomic<bool> bDataDispatchScheduled{false};
};
