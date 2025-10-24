#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "WebSocketsModule.h"
#include "IWebSocket.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

/**
 * WebRTC Signaling Client for Open3DStream
 * 
 * Handles WebSocket communication with the signaling server to exchange:
 * - Room join/leave messages
 * - SDP offers and answers
 * - ICE candidates
 * - Peer discovery
 * - Negotiation collision notifications
 */
class OPEN3DSTREAM_API FWebRTCSignalingClient
{
public:
	FWebRTCSignalingClient();
	~FWebRTCSignalingClient();

	/**
	 * Connect to the signaling server
	 * @param SignalingUrl The WebSocket URL (e.g., "ws://localhost:8080/ws")
	 * @param RoomName The room to join (e.g., "myroom")
	 * @param bIsServer Whether this is a server (true) or client (false)
	 * @return True if connection started successfully
	 */
	bool Connect(const FString& SignalingUrl, const FString& RoomName, bool bIsServer);

	/**
	 * Disconnect from the signaling server
	 */
	void Disconnect();

	/**
	 * Check if connected to signaling server
	 */
	bool IsConnected() const { return bIsConnected; }

	/**
	 * Send SDP offer to peer
	 * @param SDP The offer SDP string
	 */
	void SendOffer(const FString& SDP);

	/**
	 * Send SDP answer to peer
	 * @param SDP The answer SDP string
	 */
	void SendAnswer(const FString& SDP);

	/**
	 * Send ICE candidate to peer
	 * @param Candidate The candidate string
	 * @param SdpMid The media description line
	 * @param SdpMLineIndex The media line index
	 */
	void SendIceCandidate(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex);

	// Callbacks for signaling events
	
	/** Called when an offer is received from remote peer */
	TFunction<void(const FString& SDP)> OnOfferReceived;
	
	/** Called when an answer is received from remote peer */
	TFunction<void(const FString& SDP)> OnAnswerReceived;
	
	/** Called when an ICE candidate is received */
	TFunction<void(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex)> OnIceCandidateReceived;
	
	/** Called when peer joins the room */
	TFunction<void()> OnPeerJoined;
	
	/** Called when peer leaves the room */
	TFunction<void()> OnPeerLeft;
	
	/** Called when signaling connection is established */
	TFunction<void()> OnSignalingConnected;
	
	/** Called when signaling connection is lost */
	TFunction<void(const FString& Reason)> OnSignalingDisconnected;
	
	/** Called when there's a signaling error */
	TFunction<void(const FString& ErrorMsg)> OnSignalingError;

	/** Called when server reports an offer glare collision; provides action and suggested retry in ms */
	TFunction<void(const FString& Action, int32 RetryAfterMs)> OnCollision;

	/**
	 * Get last error message
	 */
	FString GetLastError() const { return LastError; }

private:
	// WebSocket connection
	TSharedPtr<IWebSocket> WebSocket;
	bool bIsConnected;
	FString LastError;

	// Connection details
	FString CurrentRoomName;
	bool bIsServerMode;

	// WebSocket callbacks
	void OnWebSocketConnected();
	void OnWebSocketMessage(const FString& Message);
	void OnWebSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void OnWebSocketError(const FString& Error);

	// Message parsing
	bool ParseSignalingMessage(const FString& Message);
	bool ParseOffer(const TSharedPtr<FJsonObject>& JsonMessage);
	bool ParseAnswer(const TSharedPtr<FJsonObject>& JsonMessage);
	bool ParseIceCandidate(const TSharedPtr<FJsonObject>& JsonMessage);
	bool ParsePeerJoined(const TSharedPtr<FJsonObject>& JsonMessage);
	bool ParsePeerLeft(const TSharedPtr<FJsonObject>& JsonMessage);
	bool ParseCollision(const TSharedPtr<FJsonObject>& JsonMessage);

	// Message construction
	FString CreateJoinMessage() const;
	FString CreateOfferMessage(const FString& SDP) const;
	FString CreateAnswerMessage(const FString& SDP) const;
	FString CreateIceCandidateMessage(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex) const;
};
