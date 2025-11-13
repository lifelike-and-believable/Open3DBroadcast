#pragma once

#include "CoreMinimal.h"
#include "O3DReceiverInterface.h"
#include "SocketsTransportCommon.h"

class FSocket;
class ISocketSubsystem;

/**
 * TCP receiver - client mode (connects to sender).
 * Adapted from UDP receiver pattern for reliability.
 */
class FO3DSocketsTcpReceiverNew : public IOpen3DReceiver
{
public:
	FO3DSocketsTcpReceiverNew();
	virtual ~FO3DSocketsTcpReceiverNew() override;

	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;

private:
	enum class EState : uint8
	{
		Disconnected,
		Connecting,
		Connected,
		ReadingHeader,
		ReadingPayload
	};

	bool ConnectToServer();
	bool ConnectAudioToServer();
	void DisconnectSocket();
	void DisconnectAudioSocket();
	void TickConnection();
	void TickAudioConnection();
	bool ReadFramed(FSocket* InSocket, EState& State, TArray<uint8>& Buffer, int32& InOutBytesBuffered, int32& InOutExpectedPayloadSize, TArray<uint8>& OutFrame);
	void PollAudioChannel(int32& OutFramesProcessed);
	bool ProcessAudioPayload(const TArray<uint8>& Payload);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* Socket = nullptr;
	FSocket* AudioSocket = nullptr;

	FString RemoteHost;
	int32 RemotePort = 0;
	FString StreamId;
	FString AudioRemoteHost;
	int32 AudioRemotePort = 0;

	bool bAudioEnabled = false;

	EState State = EState::Disconnected;
	EState AudioState = EState::Disconnected;

	TArray<uint8> ReceiveBuffer;
	int32 BytesBuffered = 0;
	int32 ExpectedPayloadSize = 0;

	TArray<uint8> AudioReceiveBuffer;
	int32 AudioBytesBuffered = 0;
	int32 AudioExpectedPayloadSize = 0;

	double LastConnectAttempt = 0.0;
	double LastAudioConnectAttempt = 0.0;
	int32 ConnectBackoffAttempt = 0;
	int32 AudioConnectBackoffAttempt = 0;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
};
