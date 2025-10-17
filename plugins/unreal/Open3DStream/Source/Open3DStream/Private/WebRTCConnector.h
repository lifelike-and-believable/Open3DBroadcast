// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include <memory>
#include <vector>
#include <string>

// Forward declarations
class FWebRTCSignalingClient;

namespace rtc
{
	class PeerConnection;
	class DataChannel;
	struct Configuration;
	class Candidate;
	class Description;
}

/**
 * WebRTC connector using libdatachannel
 * Provides bidirectional binary data channel communication for Open3DStream
 * 
 * Implements:
 * - WebSocket-based signaling (via FWebRTCSignalingClient)
 * - libdatachannel PeerConnection management
 * - Binary data channel for animation frames
 * - Automatic ICE/STUN negotiation
 */
class FWebRTCConnector
{
public:
	FWebRTCConnector();
	~FWebRTCConnector();

	/**
	 * Start the WebRTC connection as a client or server
	 * @param Url WebRTC URL format: webrtc://host:port/room?stun=...&turn=...
	 * @param bIsServer Whether to act as server (true) or client (false)
	 * @return True if started successfully
	 */
	bool Start(const FString& Url, bool bIsServer);

	/**
	 * Stop the WebRTC connection
	 */
	void Stop();

	/**
	 * Send data over the WebRTC data channel
	 * @param Data The data to send
	 * @param Size The size of the data
	 * @return True if sent successfully
	 */
	bool Send(const uint8* Data, int32 Size);

	/**
	 * Set the callback function for received data
	 * @param Callback The function to call when data is received
	 */
	void SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback);

	/**
	 * Get the last error message
	 */
	FString GetLastError() const { return LastError; }

	/**
	 * Check if connected
	 */
	bool IsConnected() const { return bIsConnected; }

	/**
	 * Check if data channel is open
	 */
	bool IsDataChannelOpen() const { return bDataChannelOpen; }

	/**
	 * Get connection state (for debugging)
	 */
	FString GetConnectionState() const { return ConnectionState; }

	/**
	 * Tick function to process queued messages (called from game thread)
	 */
	void Tick();

private:
	// libdatachannel objects
	std::shared_ptr<rtc::PeerConnection> PeerConnection;
	std::shared_ptr<rtc::DataChannel> DataChannel;
	std::shared_ptr<rtc::Configuration> RtcConfig;

	// Signaling
	TUniquePtr<FWebRTCSignalingClient> SignalingClient;
	FString SignalingServerUrl;
	FString RoomName;

	// State
	bool bIsConnected;
	bool bDataChannelOpen;
	bool bIsServer;
	FString ConnectionState;
	FString LastError;

	// Data callbacks
	TFunction<void(const uint8*, int32)> DataReceivedCallback;

	// Message queue (thread-safe for libdatachannel callbacks)
	TQueue<TArray<uint8>, EQueueMode::Mpsc> ReceivedDataQueue;

	// Static data channel label
	static const char* DataChannelLabel;

	// libdatachannel callback handlers (called from libdatachannel thread)
	void OnPeerConnectionStateChange(rtc::PeerConnection::State State);
	void OnDataChannelOpen();
	void OnDataChannelMessage(const std::vector<uint8>& Message);
	void OnDataChannelError(const std::string& Error);
	void OnDataChannelClosed();
	void OnIceCandidate(const rtc::Candidate& Candidate);
	void OnLocalDescription(const rtc::Description& Description);

	// Signaling callbacks
	void OnSignalingConnected();
	void OnSignalingError(const FString& Error);
	void OnSignalingDisconnected(const FString& Reason);
	void OnOfferReceived(const FString& SDP);
	void OnAnswerReceived(const FString& SDP);
	void OnIceCandidateReceived(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex);
	void OnPeerJoined();

	// Helper functions
	bool ParseWebRtcUrl(const FString& Url, FString& OutHost, uint16& OutPort, FString& OutRoom, TMap<FString, FString>& OutParams);
	bool SetupPeerConnection();
	bool CreateDataChannel();
	void CleanupPeerConnection();

	// Thread-safety
	FCriticalSection PeerConnectionLock;
	FCriticalSection DataChannelLock;
};
