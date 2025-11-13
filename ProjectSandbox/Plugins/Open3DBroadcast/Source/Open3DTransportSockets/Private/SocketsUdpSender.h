#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "SocketsTransportCommon.h"
#include "O3DAudioFrameCodec.h"

#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"

class FSocket;
class ISocketSubsystem;
class FInternetAddr;
class FSocketsUdpSenderAudioSink;

/**
 * UDP-based sender implementation for the sockets transport module.
 */
class FO3DSocketsUdpSender : public IOpen3DSender
{
public:
	FO3DSocketsUdpSender();
	virtual ~FO3DSocketsUdpSender() override;

	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual bool Send(const O3DS::SubjectList& List) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig) override;

private:
	bool ResolveRemoteAddress(const FString& Host, int32 Port);
	bool ResolveAddress(const FString& Host, int32 Port, TSharedPtr<FInternetAddr>& OutAddr);
	bool CreateSocket();
	void DestroySocket();
	bool SendPayload(FSocket* InSocket, const TSharedPtr<FInternetAddr>& InAddr, const uint8* Data, int32 Size, const TCHAR* Context);
	bool SendDatagram(FSocket* InSocket, const TSharedPtr<FInternetAddr>& InAddr, const uint8* Data, int32 Size, const TCHAR* Context);
	bool SendFragmented(FSocket* InSocket, const TSharedPtr<FInternetAddr>& InAddr, const uint8* Data, int32 Size, const TCHAR* Context);
	void RefreshAudioEncoder();
	bool ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec);
	bool SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec);

private:
	friend class FSocketsUdpSenderAudioSink;

	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> RemoteAddr;

	FString RemoteHost;
	int32 RemotePort = 0;
	FString StreamId;

	bool bAllowBroadcast = false;
	int32 MaxDatagramBytes = 64000;
	int32 MtuBytes = 1200;
	FGuid AudioSourceGuid;

	bool bAudioEncoderInitialized = false;
	O3DAudio::FFrameEncoder AudioEncoder;
	TArray<uint8> UnifiedAudioScratch;

	FThreadSafeCounter MessageCounter;
	mutable FCriticalSection SubjectNameLock;
	FString LastSubjectName;
};
