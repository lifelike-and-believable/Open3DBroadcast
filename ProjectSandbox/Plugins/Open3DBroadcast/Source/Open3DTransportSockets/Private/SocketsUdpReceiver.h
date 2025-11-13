#pragma once

#include "CoreMinimal.h"
#include "O3DReceiverInterface.h"
#include "SocketsTransportCommon.h"
#include "O3DAudioFrameCodec.h"

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
	void DestroySocket();
	bool ProcessDatagram(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState);
	bool HandleFragment(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState);
	bool IsFragmentPacket(const uint8* Data, int32 Bytes) const;
	bool ProcessReceivedPayload(const uint8* Data, int32 Size);
	bool ProcessAudioPayload(O3DS::EUnifiedCodec Codec, const uint8* Payload, int32 PayloadSize);

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* Socket = nullptr;

	FString BindHost;
	int32 BindPort = 0;
	FString StreamId;

	bool bAllowBroadcast = false;
	int32 MaxDatagramBytes = 64000;
	int32 MtuBytes = 1200;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;

	TArray<uint8> ReceiveBuffer;

	TUniquePtr<FFragmentState> FragmentState;
	O3DAudio::FFrameDecoder AudioDecoder;
	TArray<int16> DecodedPcmScratch;
};
