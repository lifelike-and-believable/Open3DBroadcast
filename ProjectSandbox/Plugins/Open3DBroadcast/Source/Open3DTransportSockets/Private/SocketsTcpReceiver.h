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
class FO3DSocketsTcpReceiver : public IOpen3DReceiver
{
public:
	FO3DSocketsTcpReceiver();
	virtual ~FO3DSocketsTcpReceiver() override;

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
	void DisconnectSocket();
	void TickConnection();
	bool ReadFramed(FSocket* InSocket, EState& State, TArray<uint8>& Buffer, int32& InOutBytesBuffered, int32& InOutExpectedPayloadSize, TArray<uint8>& OutFrame);
	bool ProcessReceivedPayload(const uint8* Data, int32 Size);
	bool ProcessAudioPayload(const uint8* Payload, int32 PayloadSize);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* Socket = nullptr;

	FString RemoteHost;
	int32 RemotePort = 0;
	FString StreamId;

	EState State = EState::Disconnected;

	TArray<uint8> ReceiveBuffer;
	int32 BytesBuffered = 0;
	int32 ExpectedPayloadSize = 0;

	double LastConnectAttempt = 0.0;
	int32 ConnectBackoffAttempt = 0;

	double LastDataReceiveTime = 0.0;

	double ConnectionTimeoutSeconds = 5.0;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
};
