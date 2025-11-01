// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "IWebRTCConnector.h"
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
 * libdatachannel-based WebRTC connector implementation
 * Provides P2P data channel communication via WebSocket signaling
 * 
 * Features:
 * - WebSocket-based signaling (via FWebRTCSignalingClient)
 * - libdatachannel PeerConnection management
 * - Binary data channel for animation frames
 * - Automatic ICE/STUN negotiation
 * - Reconnection and retry logic
 */
class FLibDataChannelConnector : public IWebRTCConnector
{
public:
	FLibDataChannelConnector();
	virtual ~FLibDataChannelConnector() override;

	// IWebRTCConnector interface
	virtual bool Start(const FString& Url, bool bIsServer) override;
	virtual void Stop() override;
	virtual bool IsConnected() const override { return bIsConnected; }
	virtual void Tick() override;

	virtual bool SendDataReliable(const uint8* Data, int32 Size) override;
	virtual bool SendDataLossy(const uint8* Data, int32 Size) override;
	virtual void SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback) override;

	virtual FString GetLastError() const override { return LastError; }

	// Audio support (placeholder for future implementation)
	virtual bool EnableAudioSend(const FAudioSendConfig& Config) override;
	virtual bool PushPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, 
	                     int32 NumChannels, int32 SampleRate, double TimestampSec) override;
	virtual FOnRemoteAudio& OnRemoteAudio() override { return RemoteAudioCallback; }

	// Additional helpers (for backward compatibility)
	bool Send(const uint8* Data, int32 Size); // Alias for SendDataLossy
	bool IsDataChannelOpen() const { return bDataChannelOpen; }
	FString GetConnectionState() const { return ConnectionState; }

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
	bool bRemoteDescriptionSet = false;
	bool bLocalDescriptionSet = false;
	FString ConnectionState;
	FString LastError;
	int32 LastPeerState = -1; // -1 = unknown

	// Retry/reconnect state
	double NextOfferTimeSeconds = 0.0;
	double OfferBackoffSeconds = 0.0;
	double NextReconnectTimeSeconds = 0.0;
	double ReconnectBackoffSeconds = 0.0;
	bool bSignalingIsConnected = false;

	// Negotiated channel options
	bool bNegotiatedChannelEnabled = false;
	int32 NegotiatedChannelId = 42;

	// Data callbacks
	TFunction<void(const uint8*, int32)> DataReceivedCallback;
	FOnRemoteAudio RemoteAudioCallback;

	// Message queue (thread-safe for libdatachannel callbacks)
	TQueue<TArray<uint8>, EQueueMode::Mpsc> ReceivedDataQueue;

	// Pending ICE candidates
	TArray<TTuple<FString, FString, int32>> PendingRemoteCandidates;

	// Static data channel label
	static const char* DataChannelLabel;

	// libdatachannel callback handlers (called from libdatachannel thread)
	void OnPeerConnectionStateChange(int State);
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
	void FlushPendingRemoteCandidates();
	void EnsurePeerConnectionForNewSession();

	// Retry/re-offer helpers
	void ResetReofferBackoff(bool bImmediate);
	void ResetReconnectBackoff(bool bImmediate);
	void MaybeCreateOffer(const TCHAR* Context);

	// Thread-safety
	FCriticalSection PeerConnectionLock;
	FCriticalSection DataChannelLock;
};
