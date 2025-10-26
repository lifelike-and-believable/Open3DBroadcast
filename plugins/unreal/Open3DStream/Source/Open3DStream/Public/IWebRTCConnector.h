// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include <memory>

/**
 * Backend-agnostic WebRTC connector interface
 * 
 * Enables swapping between P2P (libdatachannel) and LiveKit SFU without
 * changing UE components. Implementations handle signaling, connection setup,
 * data channels, and optional audio tracks.
 * 
 * Reference: docs/M3.4.1a_Docs/WEBRTC_CONNECTOR_INTERFACE.md
 */
class OPEN3DSTREAM_API IWebRTCConnector
{
public:
	virtual ~IWebRTCConnector() = default;

	// ========== Lifecycle ==========

	/**
	 * Start the WebRTC connection
	 * @param Url Connection URL (format depends on backend)
	 *            - LibDataChannel: webrtc://host:port/room?stun=...&turn=...
	 *            - LiveKit: Server URL set via config struct
	 * @param bIsServer Whether to act as server/offerer (true) or client/answerer (false)
	 * @return True if started successfully
	 */
	virtual bool Start(const FString& Url, bool bIsServer) = 0;

	/**
	 * Stop the WebRTC connection and cleanup resources
	 */
	virtual void Stop() = 0;

	/**
	 * Check if the connection is established and data can be sent
	 */
	virtual bool IsConnected() const = 0;

	/**
	 * Tick function for processing queued messages (called from game thread)
	 * Default implementation does nothing; override if needed
	 */
	virtual void Tick() {}

	// ========== Data Channels ==========

	/**
	 * Send data over reliable, ordered channel
	 * Use for: control messages, announces, critical state
	 * Keep messages small (< 15 KB) to avoid HOL blocking
	 * 
	 * @param Data Pointer to data buffer
	 * @param Size Size of data in bytes
	 * @return True if sent successfully
	 */
	virtual bool SendDataReliable(const uint8* Data, int32 Size) = 0;

	/**
	 * Send data over lossy (unreliable/unordered) channel
	 * Use for: animation frames, high-frequency updates
	 * Recommended: 2-frame queue with drop-oldest policy
	 * 
	 * @param Data Pointer to data buffer
	 * @param Size Size of data in bytes
	 * @return True if sent successfully
	 */
	virtual bool SendDataLossy(const uint8* Data, int32 Size) = 0;

	/**
	 * Set callback for received data
	 * Called on game thread (after Tick processes queued data)
	 */
	virtual void SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback) = 0;

	// ========== Audio (Optional, for future PRs) ==========

	/**
	 * Configuration for sending an audio track
	 * Subject-aware audio uses StreamLabel for routing:
	 * - Game mix: "o3ds:mix"
	 * - Per-subject mic: "o3ds:subject/<SubjectName>"
	 */
	struct FAudioSendConfig
	{
		bool bEnable = false;
		int32 SampleRate = 48000;
		int32 NumChannels = 1;
		int32 BitrateKbps = 64;
		bool bUseDTX = true;  // Discontinuous Transmission (silence suppression)

		FString StreamLabel;  // e.g., "o3ds:subject/Actor_1" or "o3ds:mix"
		FString TrackLabel;   // Unique track ID (optional; backend may auto-generate)
		FString SubjectName;  // Convenience mirror for announce/UI
		FString SourceType;   // "mic" | "mix"
	};

	/**
	 * Enable audio sending with specified configuration
	 * Default implementation returns false (audio not supported)
	 * 
	 * @param Config Audio track configuration
	 * @return True if audio track was created successfully
	 */
	virtual bool EnableAudioSend(const FAudioSendConfig& Config) { return false; }

	/**
	 * Push PCM audio samples to the specified stream
	 * Call in 10-20 ms chunks at configured sample rate
	 * Default implementation returns false (audio not supported)
	 * 
	 * @param StreamLabel Stream label from FAudioSendConfig
	 * @param Interleaved Interleaved float PCM samples [-1.0, 1.0]
	 * @param NumFrames Number of frames (samples per channel)
	 * @param NumChannels Number of channels (must match config)
	 * @param SampleRate Sample rate in Hz (must match config)
	 * @param TimestampSec Timestamp in seconds for A/V sync
	 * @return True if samples were queued successfully
	 */
	virtual bool PushPcm(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, 
	                     int32 NumChannels, int32 SampleRate, double TimestampSec) 
	{ 
		return false; 
	}

	/**
	 * Delegate for receiving remote audio (subject-aware)
	 * Params: StreamLabel, SubjectName, InterleavedPCM, NumFrames, NumChannels, SampleRate
	 * Called on game thread after Tick
	 */
	DECLARE_DELEGATE_SixParams(FOnRemoteAudio, const FString& /*StreamLabel*/, const FString& /*SubjectName*/, 
	                           const float* /*InterleavedPCM*/, int32 /*NumFrames*/, int32 /*NumChannels*/, int32 /*SampleRate*/);

	/**
	 * Get the delegate for remote audio callbacks
	 * Default implementation returns a dummy delegate
	 */
	virtual FOnRemoteAudio& OnRemoteAudio() 
	{ 
		static FOnRemoteAudio DummyDelegate;
		return DummyDelegate;
	}

	// ========== Error Handling ==========

	/**
	 * Get the last error message
	 * Useful for diagnostics when Start/Send operations fail
	 */
	virtual FString GetLastError() const { return TEXT(""); }
};

/**
 * LiveKit-specific configuration structure
 * Used when backend is LiveKit SFU
 */
struct FLiveKitConfig
{
	FString ServerUrl;  // e.g., "wss://livekit.example.com"
	FString Room;       // e.g., "room1"
	FString Token;      // JWT token for authentication
	FString Identity;   // Optional participant identity (auto-generated if empty)
};

/**
 * Factory function to create a WebRTC connector based on backend selection
 * 
 * @param Backend Backend type (LibDataChannel or LiveKit)
 * @param LiveKitConfig LiveKit configuration (required if Backend is LiveKit, ignored otherwise)
 * @return Shared pointer to IWebRTCConnector implementation, or nullptr on failure
 */
OPEN3DSTREAM_API TSharedPtr<IWebRTCConnector> CreateWebRTCConnector(
	EO3DSWebRtcBackendReceiver Backend, 
	const FLiveKitConfig* LiveKitConfig = nullptr
);
