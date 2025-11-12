#pragma once

#include "CoreMinimal.h"
#include "O3DReceiverInterface.h"
#include "SocketsTransportCommon.h"

#include "Templates/UniquePtr.h"

#include "HAL/CriticalSection.h"

class FSocket;
class ISocketSubsystem;
class FInternetAddr;

/**
 * UDP-based receiver implementation for the sockets transport module.
 */
class FO3DSocketsUdpReceiver : public IOpen3DReceiver
{
public:
	FO3DSocketsUdpReceiver();
	virtual ~FO3DSocketsUdpReceiver() override;

	virtual bool Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) override;
	virtual bool Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual bool SupportsAudio() const override;
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;

private:
	struct FFragmentState;

	bool CreateSocket();
	bool CreateAudioSocket();
	void DestroySocket();
	void DestroyAudioSocket();
	bool ProcessDatagram(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState);
	bool HandleFragment(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState);
	bool IsFragmentPacket(const uint8* Data, int32 Bytes) const;
	void PollAudioChannel(int32& OutFramesProcessed);
	bool ProcessAudioPayload(const TArray<uint8>& Payload);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* Socket = nullptr;
	FSocket* AudioSocket = nullptr;

	FString BindHost;
	int32 BindPort = 0;
	FString StreamId;
	FString AudioBindHost;
	int32 AudioBindPort = 0;

	bool bAllowBroadcast = false;
	int32 MaxDatagramBytes = 64000;
	int32 MtuBytes = 1200;
	bool bAudioEnabled = false;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;

	TArray<uint8> ReceiveBuffer;
	TArray<uint8> AudioReceiveBuffer;

	TUniquePtr<FFragmentState> FragmentState;
	TUniquePtr<FFragmentState> AudioFragmentState;

	double LastAudioDropLogTimeSeconds = 0.0;
};
