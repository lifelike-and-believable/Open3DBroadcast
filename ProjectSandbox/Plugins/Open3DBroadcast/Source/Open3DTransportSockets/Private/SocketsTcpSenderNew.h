#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "SocketsTransportCommon.h"

#include "HAL/CriticalSection.h"

class FSocket;
class ISocketSubsystem;
class FInternetAddr;
class FSocketsTcpSenderAudioSinkNew;

/**
 * TCP sender - server mode (listens and accepts connections).
 * Adapted from UDP sender pattern for reliability.
 */
class FO3DSocketsTcpSenderNew : public IOpen3DSender
{
public:
	FO3DSocketsTcpSenderNew();
	virtual ~FO3DSocketsTcpSenderNew() override;

	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
	bool CreateListenSocket();
	bool CreateAudioListenSocket();
	void DestroySocket();
	void DestroyAudioSocket();
	void TickAcceptClient();
	void TickAcceptAudioClient();
	bool SendFramed(FSocket* InSocket, const uint8* Data, int32 Size);
	bool SendAudioFrame(const FString& StreamLabel, const uint8* PCM16Data, int32 NumBytes, int32 NumChannels, int32 SampleRate, double TimestampSec);
	TSharedPtr<FInternetAddr> CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid);

private:
	friend class FSocketsTcpSenderAudioSinkNew;

	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* ListenSocket = nullptr;
	FSocket* ClientSocket = nullptr;
	FSocket* AudioListenSocket = nullptr;
	FSocket* AudioClientSocket = nullptr;

	FString BindHost;
	int32 BindPort = 0;
	FString StreamId;
	FString AudioBindHost;
	int32 AudioBindPort = 0;

	bool bAudioEnabled = false;
	FGuid AudioSourceGuid;

	double LastAcceptPollTime = 0.0;
	double LastAudioAcceptPollTime = 0.0;
	
	FCriticalSection AudioSocketLock;
};
