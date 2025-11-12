#pragma once

#include "CoreMinimal.h"
#include "O3DReceiverInterface.h"
#include "SocketsTransportCommon.h"

#include "Containers/Array.h"
#include "Templates/SharedPointer.h"

class FSocket;
class ISocketSubsystem;

/**
 * Dedicated TCP receiver implementation for the sockets transport module.
 */
class FO3DSocketsTcpReceiver : public IOpen3DReceiver
{
public:
	FO3DSocketsTcpReceiver();
	virtual ~FO3DSocketsTcpReceiver() override;

	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;

private:
	enum class EState : uint8
	{
		Sync,
		Header,
		Data
	};

	bool EnsureSocket();
	bool BeginConnect();
	void DestroySocket();
	bool ReadBytes(int32 TargetBytes);
	bool ProcessFrame(int32 PayloadSize, TArray<uint8>& OutFrame);
	void HandleSocketError();
	void ResetState();
	bool ShouldReceiveAudio() const;
	bool EnsureAudioSocket();
	bool BeginAudioConnect();
	void DestroyAudioSocket();
	bool ReadAudioBytes(int32 TargetBytes);
	bool ProcessAudioFrame(int32 PayloadSize);
	void HandleAudioSocketError();
	void ResetAudioState();
	void PollAudioChannel(int32& OutFramesProcessed);

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
	FString AudioHost;
	int32 AudioPort = 0;
	bool bAudioEnabled = false;

	double LastConnectAttempt = 0.0;
	int32 BackoffAttempt = 0;
	bool bAnnouncedConnected = false;
	double LastAudioConnectAttempt = 0.0;
	int32 AudioBackoffAttempt = 0;
	bool bAudioAnnouncedConnected = false;

	EState State = EState::Sync;
	TArray<uint8> Buffer;
	int32 BytesBuffered = 0;
	int32 ExpectedPayloadSize = 0;
	EState AudioState = EState::Sync;
	TArray<uint8> AudioBuffer;
	int32 AudioBytesBuffered = 0;
	int32 AudioExpectedPayloadSize = 0;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
};
