// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DSenderInterface.h"
#include "../Shared/SocketsTransportCommon.h"
#include "O3DAudioFrameCodec.h"
#include "O3DEncodedPayloadQueue.h"
#include "O3DLifetimeGate.h"
#include "O3DSinkAudioEncoder.h"

#include "HAL/CriticalSection.h"
#include "HAL/ThreadSafeCounter.h"

#include <atomic>

#include <vector>

class FSocket;
class ISocketSubsystem;
class FInternetAddr;
class FSocketsUdpSenderAudioSink;
class FO3DSocketsUdpSender;
class FRunnableThread;

/**
 * Publish state shared between FO3DSocketsUdpSender, its audio worker and the audio sinks it
 * hands out (ADR 0007 addendum, WP-S5: TRB-10, TRB-11). Holds no socket and no sender pointer.
 * Audio threads encode into sink-local scratch and push datagram payloads here; the audio
 * worker sends them, so the audio thread never takes the socket lock or calls SendTo.
 */
struct FSocketsUdpPublishState
{
	TSharedRef<FO3DLifetimeGate, ESPMode::ThreadSafe> Gate = MakeShared<FO3DLifetimeGate, ESPMode::ThreadSafe>();
	FO3DEncodedPayloadQueue AudioQueue{1024 * 1024};
	FO3DAudioSubjectSlot LastSubject;
	std::atomic<bool> bSocketReady{false};
	std::atomic<int64> AudioBytesQueued{0};
};

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
	virtual bool SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec) override;
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
	void StartAudioWorker();
	void StopAudioWorker();
	uint32 RunAudioWorker();

private:
	class FUdpAudioRunnable;

	FO3DTransportConfig ActiveConfig;
	FO3DTransportStats Stats;
	mutable FCriticalSection StatsMutex;
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

	FThreadSafeCounter MessageCounter;

	std::vector<char> SerializationScratch;
	std::vector<char> FragmentScratch;

	/** Guards Socket/RemoteAddr and the fragment scratch between the game thread and the audio worker. Never taken on the audio thread. */
	FCriticalSection SocketLock;

	/** Audio worker: drains PublishState->AudioQueue. Joined in Stop() before the socket is destroyed. */
	FUdpAudioRunnable* AudioWorker = nullptr;
	FRunnableThread* AudioWorkerThread = nullptr;
	std::atomic<bool> bStopAudioWorker{false};

	TSharedRef<FSocketsUdpPublishState, ESPMode::ThreadSafe> PublishState;
};
