#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "SocketsTransportCommon.h"

#include "Templates/SharedPointer.h"

class FSocket;
class ISocketSubsystem;
class FInternetAddr;
enum ESocketErrors;

class FSocketsTcpSenderAudioSink;

/**
 * Dedicated TCP sender implementation for the sockets transport module.
 */
class FO3DSocketsTcpSender : public IOpen3DSender
{
public:
	FO3DSocketsTcpSender();
	virtual ~FO3DSocketsTcpSender() override;

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
	void DestroySockets();
	void DestroyAudioSockets();
	void ResetClientSocket();
	void ResetAudioClientSocket();
	bool AcceptClient();
	bool AcceptAudioClient();
	bool SendFramedPayload(const uint8* Data, int32 Size);
	bool SendAudioFramedPayload(const uint8* Data, int32 Size);
	ESocketErrors SendBytes(FSocket* Socket, const uint8* Data, int32 Size, const TCHAR* Context);
	bool SendAudioFrame(const FString& StreamLabel, const uint8* PCM16Data, int32 NumBytes, int32 NumChannels, int32 SampleRate, double TimestampSec);
	TSharedPtr<FInternetAddr> CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid) const;

private:
	friend class FSocketsTcpSenderAudioSink;

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
	FGuid AudioSourceGuid;
	bool bAudioEnabled = false;

	double LastAcceptPollSeconds = 0.0;
	double LastAudioAcceptPollSeconds = 0.0;
	FCriticalSection AudioSocketLock;
};
