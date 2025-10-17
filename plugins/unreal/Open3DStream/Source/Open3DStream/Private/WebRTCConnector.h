// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Forward declarations for Pixel Streaming types
class IPixelStreamingModule;
class IPixelStreamingStreamer;

/**
 * WebRTC connector using Unreal's Pixel Streaming infrastructure
 * Provides bidirectional data channel communication for Open3DStream
 * 
 * This uses Unreal's native WebRTC implementation (via Pixel Streaming)
 * instead of external libraries like libdatachannel.
 */
class FWebRTCConnector
{
public:
	FWebRTCConnector();
	~FWebRTCConnector();

	/**
	 * Start the WebRTC connection as a client or server
	 * @param Url The WebRTC signaling server URL (e.g., "ws://localhost:8888")
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

private:
	/** Pixel Streaming module reference */
	IPixelStreamingModule* PixelStreamingModule;

	/** Pixel Streaming streamer instance */
	TSharedPtr<IPixelStreamingStreamer> Streamer;

	/** Data channel label for Open3DStream */
	static const FString DataChannelLabel;

	/** Whether we're connected */
	bool bIsConnected;

	/** Whether we're acting as server */
	bool bIsServer;

	/** Last error message */
	FString LastError;

	/** Callback for received data */
	TFunction<void(const uint8*, int32)> DataReceivedCallback;

	/** Delegate handle for data channel messages */
	FDelegateHandle DataChannelMessageHandle;

	/** Delegate handle for connection state changes */
	FDelegateHandle ConnectionStateHandle;

	/** Handle data received from data channel */
	void OnDataChannelMessage(FString PlayerId, uint8 MessageType, TArray<uint8> DataBuffer);

	/** Handle streamer connection */
	void OnStreamerConnected();

	/** Handle streamer disconnection */
	void OnStreamerDisconnected();
