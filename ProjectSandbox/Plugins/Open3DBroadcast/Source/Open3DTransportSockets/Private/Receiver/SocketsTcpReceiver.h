// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "../Shared/SocketsTransportCommon.h"
#include "O3DAudioFrameCodec.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/tcp_stream_parser.h"
THIRD_PARTY_INCLUDES_END

class FSocket;
class ISocketSubsystem;

/**
 * TCP receiver - client mode (connects to sender).
 *
 * Everything runs on the thread that calls Poll() (the game thread): the socket is
 * non-blocking and each Poll() does bounded work. Framing is parsed by the core
 * O3DS::TcpStreamParser (WP-S6).
 */
class FO3DSocketsTcpReceiver : public IOpen3DReceiver
{
public:
	FO3DSocketsTcpReceiver();
	virtual ~FO3DSocketsTcpReceiver() override;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DSockets::GetTcpCapabilities(FO3DTransportConfig()); }
	/** Connecting until the connection is up, Connected while it is, Reconnecting after it dropped. Changes happen in Poll (game thread). */
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
	virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { ControlSink = Sink; }

	/** True while a TCP connection to the sender is established. For tests and diagnostics. */
	bool IsConnected() const { return Socket != nullptr && State == EState::Connected; }

	/** Number of connections established since Start(). For tests and diagnostics. */
	int32 GetConnectCount() const { return ConnectCount; }

private:
	enum class EState : uint8
	{
		Disconnected,
		Connecting,
		Connected
	};

	bool ConnectToServer();
	void DisconnectSocket();
	void TickConnection();
	/** Returns false when the peer closed the connection or it failed. */
	bool ReadAvailable(int32& InOutFramesProcessed, int64& InOutBytesRead);
	bool ProcessReceivedPayload(const uint8* Data, int32 Size);
	bool ProcessAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize);
	void ReportParserStats();
	double GetBackoffSeconds() const;

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* Socket = nullptr;
	bool bRunning = false;

	FString RemoteHost;
	int32 RemotePort = 0;
	FString StreamId;

	EState State = EState::Disconnected;

	/** Socket-free framing (TRB-1, TRB-8, TRB-9). Reset on every new connection. */
	O3DS::TcpStreamParser Parser;
	uint64 ReportedDiscardedBytes = 0;
	uint64 ReportedRejectedFrames = 0;
	bool bWarnedResyncThisConnection = false;

	double LastConnectAttempt = 0.0;
	double ConnectStartTime = 0.0;
	int32 ConnectBackoffAttempt = 0;
	int32 ConnectCount = 0;
	bool bReceivedOnThisConnection = false;

	double LastDataReceiveTime = 0.0;

	double ConnectionTimeoutSeconds = 5.0;
	double ConnectTimeoutSeconds = 5.0;
	double InitialBackoffSeconds = 0.5;
	double MaxBackoffSeconds = 5.0;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
	/** Control payloads (ADR 0011). Held strongly, released in Stop; used only from Poll (game thread). */
	TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ControlSink;
	O3DAudio::FMultiStreamFrameDecoder AudioDecoder; // SHR-15: one decoder per (SourceGuid, StreamLabel)
	TArray<int16> DecodedPcmScratch;
	/** ADR 0007 item 3. */
	FO3DConnectionStateTracker ConnectionState;
};
