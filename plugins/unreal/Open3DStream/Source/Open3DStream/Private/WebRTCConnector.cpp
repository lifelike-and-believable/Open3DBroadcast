// Copyright Epic Games, Inc. All Rights Reserved.

#include "WebRTCConnector.h"
#include "IPixelStreamingModule.h"
#include "IPixelStreamingStreamer.h"
#include "Modules/ModuleManager.h"

const FString FWebRTCConnector::DataChannelLabel = TEXT("Open3DStream");

FWebRTCConnector::FWebRTCConnector()
	: PixelStreamingModule(nullptr)
	, bIsConnected(false)
	, bIsServer(false)
{
}

FWebRTCConnector::~FWebRTCConnector()
{
	Stop();
}

bool FWebRTCConnector::Start(const FString& Url, bool bInIsServer)
{
	Stop();

	bIsServer = bInIsServer;
	LastError.Empty();

	// Get Pixel Streaming module
	if (!FModuleManager::Get().IsModuleLoaded("PixelStreaming"))
	{
		LastError = TEXT("PixelStreaming module is not loaded. Please enable the PixelStreaming plugin.");
		return false;
	}

	PixelStreamingModule = &IPixelStreamingModule::Get();
	if (!PixelStreamingModule)
	{
		LastError = TEXT("Failed to get PixelStreaming module interface");
		return false;
	}

	// Get or create the default streamer
	FString StreamerId = TEXT("Open3DStream");
	Streamer = PixelStreamingModule->FindStreamer(StreamerId);
	if (!Streamer.IsValid())
	{
		// Create a new streamer for Open3DStream
		Streamer = PixelStreamingModule->CreateStreamer(StreamerId);
		if (!Streamer.IsValid())
		{
			LastError = TEXT("Failed to create Pixel Streaming streamer");
			return false;
		}
	}

	// Register protocol for custom data channel
	// This creates a data channel that both sides can use for bidirectional communication
	Streamer->AddInputComponent("Open3DStream");
	
	// Set up data channel message callback
	DataChannelMessageHandle = Streamer->OnDataChannelMessage().AddRaw(this, &FWebRTCConnector::OnDataChannelMessage);

	// Set up connection callbacks
	ConnectionStateHandle = Streamer->OnStreamingStarted().AddRaw(this, &FWebRTCConnector::OnStreamerConnected);
	Streamer->OnStreamingStopped().AddRaw(this, &FWebRTCConnector::OnStreamerDisconnected);

	// Start the streamer
	// Note: Actual signaling connection is managed by Pixel Streaming's configuration
	// The streamer will connect to the signaling server specified in the Pixel Streaming settings
	Streamer->StartStreaming();

	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector started (Mode: %s, Streamer: %s)"),
		bIsServer ? TEXT("Server") : TEXT("Client"), *StreamerId);
	UE_LOG(LogTemp, Log, TEXT("Connect via Pixel Streaming signaling server at: %s"), *Url);

	return true;
}

void FWebRTCConnector::Stop()
{
	if (Streamer.IsValid())
	{
		// Remove callbacks
		if (DataChannelMessageHandle.IsValid())
		{
			Streamer->OnDataChannelMessage().Remove(DataChannelMessageHandle);
			DataChannelMessageHandle.Reset();
		}
		
		if (ConnectionStateHandle.IsValid())
		{
			Streamer->OnStreamingStarted().Remove(ConnectionStateHandle);
			ConnectionStateHandle.Reset();
		}

		// Stop streaming
		Streamer->StopStreaming();
		Streamer.Reset();
	}

	PixelStreamingModule = nullptr;
	bIsConnected = false;
	DataReceivedCallback = nullptr;
}

bool FWebRTCConnector::Send(const uint8* Data, int32 Size)
{
	if (!bIsConnected)
	{
		LastError = TEXT("Not connected");
		return false;
	}

	if (!Streamer.IsValid())
	{
		LastError = TEXT("Streamer not available");
		return false;
	}

	// Send data over the data channel to all connected players
	// MessageType 0 = binary data for Open3DStream protocol
	TArray<uint8> DataArray;
	DataArray.Append(Data, Size);
	
	Streamer->SendPlayerMessage(0, DataArray);

	return true;
}

void FWebRTCConnector::SetDataReceivedCallback(TFunction<void(const uint8*, int32)> Callback)
{
	DataReceivedCallback = Callback;
}

void FWebRTCConnector::OnDataChannelMessage(FString PlayerId, uint8 MessageType, TArray<uint8> DataBuffer)
{
	// Invoke the callback with received data
	// MessageType 0 is reserved for Open3DStream binary protocol data
	if (MessageType == 0 && DataReceivedCallback)
	{
		DataReceivedCallback(DataBuffer.GetData(), DataBuffer.Num());
	}
}

void FWebRTCConnector::OnStreamerConnected()
{
	bIsConnected = true;
	UE_LOG(LogTemp, Log, TEXT("WebRTC Connector: Streamer connected"));
}

void FWebRTCConnector::OnStreamerDisconnected()
{
	bIsConnected = false;
	UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Streamer disconnected"));
}
