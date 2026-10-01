// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DReceiverInterface.h"
#include "../Shared/SocketsTransportCommon.h"
#include "O3DAudioFrameCodec.h"

#include "Templates/UniquePtr.h"
#include "Templates/SharedPointer.h"

#include <vector>

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
	virtual bool SupportsControl() const override { return true; }
	virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { ControlSink = Sink; }

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
	int32 MaxFrameBytes = 0;

	TWeakPtr<ISerializedFrameConsumer> Consumer;
	TWeakPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> AudioSink;
	/** Control payloads (ADR 0011). Held strongly, released in Stop; used only from Poll (game thread). */
	TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe> ControlSink;

	/** Sized to the largest possible UDP datagram, independent of udp.maxdatagram (TRB-23). */
	TArray<uint8> ReceiveBuffer;
	/** Reused across datagrams to avoid per-datagram allocation (TRB-19). */
	TSharedPtr<FInternetAddr> RecvAddr;
	TArray<uint8> FrameScratch;
	std::vector<char> CombinedScratch;

	TUniquePtr<FFragmentState> FragmentState;
	O3DAudio::FMultiStreamFrameDecoder AudioDecoder; // SHR-15: one decoder per (SourceGuid, StreamLabel)
	TArray<int16> DecodedPcmScratch;
};
