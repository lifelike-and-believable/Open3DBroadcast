// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DLogThrottle.h"
#include "Transport/O3DReceiverInterface.h"
#include "Transport/O3DConnectionState.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DUnifiedReceiveDemux.h"
#include "../Shared/SocketsTransportCommon.h"

#include "Templates/UniquePtr.h"
#include "Templates/SharedPointer.h"

#include <vector>

#include "HAL/CriticalSection.h"

class FSocket;
class ISocketSubsystem;
class FInternetAddr;

/**
 * UDP receiver. Poll() (game thread, bounded per call; TRB-18) reads datagrams, reassembles
 * fragments with the core's udp_fragment (v2 header, ADR 0009 item 5) and hands each complete message
 * to the shared FO3DUnifiedReceiveDemux (ADR 0007 item 7, WP-A1 PR 4c), which calls the consumer,
 * the audio sink and the control sink. Receive-side threading stays on Poll (ADR 0007
 * "Not decided here"). The bind address must be an IP literal or a wildcard, so binding resolves
 * no name on the game thread.
 */
class FO3DSocketsUdpReceiver : public IOpen3DReceiver
{
public:
	FO3DSocketsUdpReceiver();
	virtual ~FO3DSocketsUdpReceiver() override;

	virtual FO3DTransportResult Initialize(const FO3DTransportConfig& Config) override;
	virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer) override { Demux.SetConsumer(InConsumer); }
	virtual FO3DTransportResult Start() override;
	virtual void Stop() override;
	virtual int32 Poll() override;
	virtual FO3DTransportStats GetStats() const override;
	virtual FO3DTransportCapabilities GetCapabilities() const override { return O3DSockets::GetUdpCapabilities(FO3DTransportConfig()); }
	/** Connected from a successful Start (the socket is bound) to Stop; UDP has no connection to lose. */
	virtual EO3DConnectionState GetConnectionState() const override { return ConnectionState.Get(); }
	virtual void SetStateChangedCallback(FO3DConnectionStateCallback Callback) override { ConnectionState.SetCallback(MoveTemp(Callback)); }
	virtual void SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig) override;
	virtual void SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink) override { Demux.SetControlSink(Sink); }

private:
	struct FFragmentState;

	FO3DTransportResult CreateSocket();
	void DestroySocket();
	bool ProcessDatagram(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState);
	bool HandleFragment(const uint8* Data, int32 Bytes, TArray<uint8>& OutFrame, TUniquePtr<FFragmentState>& InState);
	bool IsFragmentPacket(const uint8* Data, int32 Bytes) const;

private:
	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	FO3DTransportAudioConfig ActiveAudioConfig;

	ISocketSubsystem* SocketSubsystem = nullptr;
	FSocket* Socket = nullptr;

	FO3DHostPort BindEndpoint;
	int32 BindPort = 0;
	FString StreamId;

	bool bAllowBroadcast = false;
	int32 MaxDatagramBytes = 64000;
	int32 MaxFrameBytes = 0;
	// WP-R3 (TR-6): hot-path log sites, one throttle each, per instance.
	FO3DLogThrottle UdpRecvFailedLog;
	FO3DLogThrottle UdpOversizeLog;


	/** Holds the consumer, audio sink and control sink strongly; Stop() releases them (TRF-38, ADR 0011). */
	FO3DUnifiedReceiveDemux Demux;

	/** Sized to the largest possible UDP datagram, independent of udp.maxdatagram (TRB-23). */
	TArray<uint8> ReceiveBuffer;
	/** Reused across datagrams to avoid per-datagram allocation (TRB-19). */
	TSharedPtr<FInternetAddr> RecvAddr;
	TArray<uint8> FrameScratch;
	std::vector<char> CombinedScratch;

	TUniquePtr<FFragmentState> FragmentState;
	/** ADR 0007 item 3. */
	FO3DConnectionStateTracker ConnectionState;
};
