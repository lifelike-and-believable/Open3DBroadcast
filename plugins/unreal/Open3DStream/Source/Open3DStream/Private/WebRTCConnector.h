// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include <memory>
#include <vector>
#include <string>

// Forward declare Opus types when Opus support is enabled so we can hold pointers
#if O3DS_WITH_OPUS
struct OpusEncoder;
struct OpusDecoder;
#endif

// Forward declarations
class FWebRTCSignalingClient;

namespace rtc
{
	class PeerConnection;
	class DataChannel;
	struct Configuration;
	class Candidate;
	class Description;
	class Track;
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
class FWebRTCConnector : public TSharedFromThis<FWebRTCConnector>
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

	// ====== Audio (Opus over WebRTC track) ======
	struct FAudioConfig
	{
		int32 SampleRate =48000;
		int32 NumChannels =1; //1 mono,2 stereo
		int32 BitrateKbps =32; // target encoder bitrate
		int32 FrameSizeMs =20; //10/20/40 typical
		FString StreamLabel; // optional desired msid label
	};

	// Enable Opus-encoded audio sending via a WebRTC audio track
	void EnableAudioSend(const FAudioConfig& InConfig);
	void DisableAudioSend();
	// Push interleaved PCM16 samples (NumSamples is total across channels)
	bool PushAudioPCM16(const int16* Samples, int32 NumSamples);

	// Set callback for decoded PCM16 receive (called on game thread from Tick)
	void SetAudioReceiveCallback(TFunction<void(const int16* PCM, int32 NumSamples, int32 NumChannels, int32 SampleRate)> Callback);

	// Remote audio callback: StreamLabel, SubjectName, PCM float interleaved samples, NumFrames, NumChannels, SampleRate
	DECLARE_MULTICAST_DELEGATE_SixParams(FOnRemoteAudioFloat, const FString& /*StreamLabel*/, const FString& /*SubjectName*/, const float* /*PCM*/, int32 /*NumFrames*/, int32 /*NumChannels*/, int32 /*SampleRate*/);
	FOnRemoteAudioFloat& OnRemoteAudio() { return RemoteAudioDelegate; }

	// Configure RX audio routing labels (set by higher layers parsing announce/SDP)
	void SetRxAudioRouting(const FString& InStreamLabel, const FString& InSubjectName)
	{
		RxStreamLabel = InStreamLabel;
		RxSubjectName = InSubjectName;
	}

	// Active connector accessor (optional registry)
	static void SetActiveConnector(const TSharedPtr<FWebRTCConnector>& InConnector);
	static TSharedPtr<FWebRTCConnector> GetActiveConnector();

private:
	// libdatachannel objects
	std::shared_ptr<rtc::PeerConnection> PeerConnection;
	std::shared_ptr<rtc::DataChannel> DataChannel;
	std::shared_ptr<rtc::Configuration> RtcConfig;
	std::shared_ptr<rtc::Track> AudioTrack;

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

	// Pending remote ICE candidates until both descriptions are set
	TArray<TTuple<FString, FString, int32>> PendingRemoteCandidates;

	// Audio state
	bool bAudioSendEnabled = false;
	struct FAudioRuntime
	{
		FAudioConfig Config;
		uint32 RTPClockHz =48000; // Opus RTP clock
		uint32 Timestamp =0; // RTP timestamp counter
		int32 FrameSizeSamples =960; //20ms @48k
		TArray<int16> Pending;
	} AudioRt;
	TFunction<void(const int16*, int32, int32, int32)> AudioRxCallback;
	struct FRxBuffer { TArray<int16> PCM; int32 NumChannels=1; int32 SampleRate=48000; };
	TQueue<FRxBuffer, EQueueMode::Mpsc> DecodedPcmQueue;

	// Internal helpers
	void EnsurePeerConnectionForNewSession();
	bool SetupPeerConnection();
	bool CreateDataChannel();
	void CleanupPeerConnection();
	void FlushPendingRemoteCandidates();

	// Event handlers
	void OnPeerConnectionStateChange(int StateInt);
	void OnDataChannelOpen();
	void OnDataChannelMessage(const std::vector<uint8>& Message);
	void OnDataChannelError(const std::string& Error);
	void OnDataChannelClosed();
	void OnIceCandidate(const rtc::Candidate& Candidate);
	void OnLocalDescription(const rtc::Description& Description);
	void OnSignalingConnected();
	void OnSignalingError(const FString& Error);
	void OnSignalingDisconnected(const FString& Reason);
	void OnOfferReceived(const FString& SDP);
	void OnAnswerReceived(const FString& SDP);
	void OnIceCandidateReceived(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex);
	void OnPeerJoined();

	bool ParseWebRtcUrl(const FString& Url, FString& OutHost, uint16& OutPort, FString& OutRoom, TMap<FString, FString>& OutParams);

	void ResetReofferBackoff(bool bImmediate);
	void ResetReconnectBackoff(bool bImmediate);
	void MaybeCreateOffer(const TCHAR* Context);

	// Threading
	FCriticalSection PeerConnectionLock;
	FCriticalSection DataChannelLock;

	// Re-offer/reconnect timers
	bool bSignalingIsConnected = false;
	double NextOfferTimeSeconds =0.0;
	double OfferBackoffSeconds =0.0;
	double NextReconnectTimeSeconds =0.0;
	double ReconnectBackoffSeconds =0.0;
	int32 LastPeerState;

	// Negotiated channel support
	bool bNegotiatedChannelEnabled = false;
	int32 NegotiatedChannelId =42;

	// Data receive queue
	TQueue<TArray<uint8>, EQueueMode::Mpsc> ReceivedDataQueue;
	TFunction<void(const uint8*, int32)> DataReceivedCallback;
	FString LastError;

	static const char* DataChannelLabel;

	// Subject-aware remote audio multicast
	FOnRemoteAudioFloat RemoteAudioDelegate;

	// RX routing (defaults)
	FString RxStreamLabel = TEXT("o3ds:mix");
	FString RxSubjectName;

	// Optional global active connector registry
	static TWeakPtr<FWebRTCConnector> ActiveConnector;

#if O3DS_WITH_OPUS
	// Opus encoder/decoder handles (pointers only; full types provided in cpp)
	OpusEncoder* OpusEnc = nullptr;
	OpusDecoder* OpusDec = nullptr;
	void DestroyOpus();
	bool EnsureOpusEncoder(const FAudioConfig& In);
	bool EnsureOpusDecoder(int32 SampleRate, int32 NumChannels);
#endif
};
