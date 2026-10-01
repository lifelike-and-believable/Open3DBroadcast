// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTcpSender.h"
#include "../Shared/SocketsTcpAudio.h"
#include "../Shared/SocketsTcpTransport.h"
#include "O3DSenderAudioSinkBase.h"
#include "O3DSinkAudioEncoder.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DUnifiedMessage.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "Misc/ScopeLock.h"
#include "Logging/LogMacros.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpSender, Log, All);

class FO3DSocketsTcpSender::FTcpSenderRunnable final : public FRunnable
{
public:
	explicit FTcpSenderRunnable(FO3DSocketsTcpSender& InOwner)
		: Owner(InOwner)
	{
	}

	virtual uint32 Run() override
	{
		return Owner.RunWorker();
	}

	virtual void Stop() override
	{
		// Owner drives stop via atomics; nothing required here.
	}

private:
	FO3DSocketsTcpSender& Owner;
};

namespace
{
	/**
	 * Each queue item is [enqueue time as a double][TCP frame header][payload]. The time lets
	 * the worker drop frames that waited longer than tcp.maxqueueage (TRB-14) without a second
	 * queue type; the prefix is stripped before sending and never goes on the wire.
	 */
	constexpr int32 QueueItemPrefixBytes = static_cast<int32>(sizeof(double));

	/** Build a queue item for a payload. Safe on any thread. */
	TArray<uint8> MakeQueuedFrame(const uint8* Data, int32 Size)
	{
		TArray<uint8> Item;
		const int32 HeaderSize = O3DSockets::Tcp::FrameHeaderSize;
		Item.SetNumUninitialized(QueueItemPrefixBytes + HeaderSize + Size);
		const double Now = FPlatformTime::Seconds();
		FMemory::Memcpy(Item.GetData(), &Now, sizeof(double));
		O3DSockets::Tcp::WriteFrameHeader(Item.GetData() + QueueItemPrefixBytes, Size);
		FMemory::Memcpy(Item.GetData() + QueueItemPrefixBytes + HeaderSize, Data, Size);
		return Item;
	}

	/** True when a queue item carries a control envelope (ADR 0011), which is never counted as a frame. */
	bool IsControlItem(const TArray<uint8>& Item)
	{
		const int32 Offset = QueueItemPrefixBytes + O3DSockets::Tcp::FrameHeaderSize;
		TConstArrayView<uint8> Payload;
		return Item.Num() > Offset && O3DS::TryGetControlPayload(Item.GetData() + Offset, Item.Num() - Offset, Payload);
	}

	double ReadEnqueueTime(const TArray<uint8>& Item)
	{
		double Time = 0.0;
		if (Item.Num() >= QueueItemPrefixBytes)
		{
			FMemory::Memcpy(&Time, Item.GetData(), sizeof(double));
		}
		return Time;
	}

	/** Worker waits: short, so Stop() never waits long for the join. */
	constexpr uint32 AcceptPollMs = 10;
	constexpr uint32 MaxIdleWaitMs = 50;
	constexpr double WriteWaitSeconds = 0.010;
	/** How often an idle worker checks whether the receiver closed the connection (TRB-6). */
	constexpr double PeerCheckIntervalSeconds = 0.25;
}

/**
 * TCP audio sink (WP-S5: TRB-10, TRB-11). Encodes on the calling (audio) thread with its own
 * encoders and hands framed bytes to the worker through the shared queue. It never takes the
 * socket lock and never references the sender.
 */
class FSocketsTcpSenderAudioSink final : public FO3DGatedSenderAudioSink
{
public:
	FSocketsTcpSenderAudioSink(TSharedRef<FSocketsTcpPublishState, ESPMode::ThreadSafe> InState, FO3DTransportAudioConfig InConfig, FO3DSinkAudioEncoder::FSettings InEncoderSettings)
		: FO3DGatedSenderAudioSink(MoveTemp(InConfig), InState->Gate, MoveTemp(InEncoderSettings))
		, State(MoveTemp(InState))
	{
	}

protected:
	virtual bool OnSubmitGated(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec) override
	{
		if (!State->bClientConnected.load())
		{
			return false;
		}

		// Call-local scratch: a sink may be fed from more than one thread (TRF-40). Opus may
		// return zero or several packets per buffer (SHR-2).
		TArray<TArray<uint8>> Messages;
		if (!GetEncoder().EncodeUnified(StreamLabel, FString(), Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Messages))
		{
			return false;
		}

		bool bAllQueued = true;
		for (const TArray<uint8>& Unified : Messages)
		{
			const int64 Size = Unified.Num();
			if (!State->SendQueue.Enqueue(MakeQueuedFrame(Unified.GetData(), Unified.Num())))
			{
				UE_LOG(LogSocketsTcpSender, Verbose, TEXT("TCP sender failed to enqueue audio frame"));
				bAllQueued = false;
				continue;
			}
			State->AudioBytesQueued.fetch_add(Size);
		}
		return bAllQueued;
	}

private:
	TSharedRef<FSocketsTcpPublishState, ESPMode::ThreadSafe> State;
};

FO3DSocketsTcpSender::FO3DSocketsTcpSender()
	: KeepaliveFrame(O3DSockets::Tcp::MakeKeepaliveFrame())
	, PublishState(MakeShared<FSocketsTcpPublishState, ESPMode::ThreadSafe>())
{
	MaxQueueBytes = static_cast<uint64>(O3DSockets::Tcp::DefaultMaxQueueBytes);
	PublishState->SendQueue.SetMaxBytes(MaxQueueBytes);
}

FO3DSocketsTcpSender::~FO3DSocketsTcpSender()
{
	// Stop() closes the audio gate first, so every audio-thread submit already inside a sink
	// has returned before the worker and sockets go away.
	Stop();
}

FO3DTransportResult FO3DSocketsTcpSender::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	{
		FScopeLock Lock(&StatsMutex);
		Stats.Reset();
	}
	StreamId = ActiveConfig.StreamId;

	ActiveAudioConfig = Config.Audio;
	// Note: Audio stream label is now automatically derived from StreamId
	AudioSourceGuid = FGuid::NewGuid();
	PublishState->AudioBytesQueued.store(0);
	SendWaitCount.store(0);

	// Parse bind address from config
	if (!O3DSockets::ParseHostPort(Config, BindHost, BindPort, TEXT("tcp")))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender requires tcp://host:port URI or explicit host/port options."));
		BindPort = 0;
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("TCP sender requires a tcp://host:port URI or explicit host/port options."));
	}

	if (BindPort <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender requires a valid port (got %d)."), BindPort);
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("TCP sender requires a valid port (got %d)."), BindPort));
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(BindHost, BindPort);
		ActiveConfig.StreamId = StreamId;
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender could not access socket subsystem."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("TCP sender could not access the socket subsystem."));
	}

	namespace Tcp = O3DSockets::Tcp;

	// TRB-14: configurable queue cap and age limit.
	MaxQueueBytes = static_cast<uint64>(FMath::Max(O3DSockets::GetIntOption(Config, Tcp::MaxQueueOptionKey, Tcp::DefaultMaxQueueBytes), Tcp::MinQueueBytes));
	PublishState->SendQueue.SetMaxBytes(MaxQueueBytes);
	MaxQueueAgeSeconds = FMath::Max(0, O3DSockets::GetIntOption(Config, Tcp::MaxQueueAgeOptionKey, Tcp::DefaultMaxQueueAgeMs)) / 1000.0;
	// TRB-2: how long a frame may make no progress before the client is dropped.
	StallTimeoutSeconds = FMath::Max(O3DSockets::GetIntOption(Config, Tcp::StallTimeoutOptionKey, Tcp::DefaultStallTimeoutMs), Tcp::MinStallTimeoutMs) / 1000.0;
	// TRB-6: keepalive interval while idle.
	KeepaliveIntervalSeconds = FMath::Max(0, O3DSockets::GetIntOption(Config, Tcp::KeepaliveOptionKey, Tcp::DefaultKeepaliveMs)) / 1000.0;

	PublishState->Gate->Open();

	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DSocketsTcpSender::Start()
{
	// Restart: stop a running worker before touching the sockets it owns.
	bRunning.store(false);
	if (WorkerThread)
	{
		bStopWorker = true;
		PublishState->SendQueue.Wake();
		StopWorker();
	}
	DestroySocket();
	DrainQueue();

	// TRB-13: Stop() keeps SocketSubsystem; fetch it again only if Initialize() never ran.
	if (!SocketSubsystem)
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}
	if (!SocketSubsystem || BindPort <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender cannot start: not initialized."));
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("TCP sender Start() before a successful Initialize()."));
	}

	// TRB-13: create the listen socket first, and start the worker only if that worked, so a
	// failed Start() leaves no thread running.
	const FO3DTransportResult ListenResult = CreateListenSocket();
	if (!ListenResult.IsOk())
	{
		ConnectionState.End(EO3DConnectionState::Failed, ListenResult);
		return ListenResult;
	}

	PublishState->Gate->Open();

	// Listening, no receiver yet. Before the worker starts, so its "Connected" on accept follows.
	ConnectionState.Begin(EO3DConnectionState::Connecting);

	bStopWorker = false;
	if (!StartWorker())
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender could not start its worker thread."));
		DestroySocket();
		const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("TCP sender could not start its worker thread."));
		ConnectionState.End(EO3DConnectionState::Failed, Result);
		return Result;
	}
	bRunning.store(true);
	return FO3DTransportResult::Ok();
}

void FO3DSocketsTcpSender::Stop()
{
	// WP-S5 ordering: (1) close the audio gate, which waits for in-flight submits;
	// (2) stop and join the worker; (3) destroy sockets; (4) drain. The wake event is owned
	// by the shared queue, so a late Wake() can never hit a pooled event (TRB-12).
	bRunning.store(false);
	PublishState->Gate->Close();

	bStopWorker = true;
	PublishState->SendQueue.Wake();

	StopWorker();

	DestroySocket();
	DrainQueue();
	// SocketSubsystem is kept so Start() works again without Initialize() (TRB-13).

	// The worker has been joined, so nothing reports a change after this.
	ConnectionState.End(EO3DConnectionState::Idle);
}

bool FO3DSocketsTcpSender::Send(const O3DS::SubjectList& List)
{
	// Fast path: check connection state without locks
	if (!bRunning.load() || !PublishState->bClientConnected.load())
	{
		// Not started, or no client connected yet
		return false;
	}

	const double Timestamp = FPlatformTime::Seconds();

	// Reuse serialization buffer instead of allocating std::vector every frame
	// This avoids malloc/free overhead and heap fragmentation at 60fps
	SerializationScratch.clear();
	int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(SerializationScratch, Timestamp);
	if (BytesWritten <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Verbose, TEXT("TCP sender failed to serialize SubjectList."));
		FScopeLock Lock(&StatsMutex);
		Stats.DroppedFrames++;
		return false;
	}

	return SendBytes(reinterpret_cast<const uint8*>(SerializationScratch.data()), BytesWritten) == EO3DSendResult::Queued;
}

EO3DSendResult FO3DSocketsTcpSender::SendSerialized(FO3DSendPayload&& Payload)
{
	if (!bRunning.load())
	{
		return EO3DSendResult::NotRunning;
	}
	const int32 Len = Payload.Bytes.Num();
	if (Len <= 0)
	{
		return EO3DSendResult::Invalid;
	}
	if (Len > O3DSockets::Tcp::MaxFrameBytesLimit)
	{
		return EO3DSendResult::TooLarge;
	}
	if (!PublishState->bClientConnected.load())
	{
		return EO3DSendResult::NotConnected;
	}

	// The frame is copied once more into the framed queue item (header and enqueue time in front);
	// the shared send queue of WP-A1 step 4 takes the payload as it is.
	return SendBytes(Payload.Bytes.GetData(), Len);
}

/**
 * Control (ADR 0011): the envelope rides the frame queue in-band, as audio does, and the worker
 * sends it in order with the frames around it. Not counted as a frame. Refused without a client
 * (NotConnected), like SendSerialized; the control publisher retries, and its snapshot reaches a
 * late client.
 */
EO3DSendResult FO3DSocketsTcpSender::SendControl(const uint8* Envelope, int32 Len)
{
	if (!bRunning.load())
	{
		return EO3DSendResult::NotRunning;
	}
	TConstArrayView<uint8> Payload;
	if (!O3DS::TryGetControlPayload(Envelope, Len, Payload))
	{
		return EO3DSendResult::Invalid;
	}
	if (!PublishState->bClientConnected.load())
	{
		return EO3DSendResult::NotConnected;
	}
	return EnqueuePayload(Envelope, Len) ? EO3DSendResult::Queued : EO3DSendResult::DroppedBackpressure;
}

/** Enqueue already-serialized bytes for transmission and record transport-level stats. */
EO3DSendResult FO3DSocketsTcpSender::SendBytes(const uint8* Data, int32 Len)
{
	if (!EnqueuePayload(Data, Len))
	{
		FScopeLock Lock(&StatsMutex);
		Stats.DroppedFrames++;
		return EO3DSendResult::DroppedBackpressure;
	}

	{
		FScopeLock Lock(&StatsMutex);
		Stats.FramesSent++;
		Stats.BytesSent += Len;
	}
	return EO3DSendResult::Queued;
}

void FO3DSocketsTcpSender::Tick(float /*DeltaSeconds*/)
{
	// Accepting, sending, keepalives and peer-close detection all run on the worker (WP-S6),
	// so the game thread never waits on a socket or on the worker.
}

FO3DTransportStats FO3DSocketsTcpSender::GetStats() const
{
	FO3DTransportStats Copy;
	{
		FScopeLock Lock(&StatsMutex);
		Copy = Stats;
	}
	Copy.BytesSent += PublishState->AudioBytesQueued.load();
	Copy.State = ConnectionState.Get();
	Copy.PendingBytes = static_cast<int64>(PublishState->SendQueue.GetPendingBytes());
	return Copy;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DSocketsTcpSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
	FO3DTransportAudioConfig EffectiveConfig = ActiveAudioConfig;
	if (AudioConfig.bEnableAudio)
	{
		EffectiveConfig = AudioConfig;
	}

	EffectiveConfig.bEnableAudio = true;
	EffectiveConfig.NumChannels = FMath::Max(EffectiveConfig.NumChannels, 1);
	EffectiveConfig.SampleRate = FMath::Max(EffectiveConfig.SampleRate, 1);
	// Note: Audio stream label is now automatically derived from StreamId

	ActiveAudioConfig = EffectiveConfig;

	// Immutable snapshot for this sink's own encoders (TRB-11): nothing reconfigures them later.
	const FString StreamFallback = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	FO3DSinkAudioEncoder::FSettings EncoderSettings;
	EncoderSettings.Config = ActiveAudioConfig;
	EncoderSettings.DefaultStreamLabel = StreamFallback;
	EncoderSettings.DefaultSubject = StreamFallback;
	EncoderSettings.SourceGuid = AudioSourceGuid;

	return MakeShared<FSocketsTcpSenderAudioSink, ESPMode::ThreadSafe>(PublishState, ActiveAudioConfig, MoveTemp(EncoderSettings));
}

FO3DTransportResult FO3DSocketsTcpSender::CreateListenSocket()
{
	if (!SocketSubsystem)
	{
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("TCP sender has no socket subsystem."));
	}

	bool bValid = false;
	TSharedPtr<FInternetAddr> BindAddr = CreateBindAddress(BindHost, BindPort, bValid);
	if (!bValid || !BindAddr.IsValid())
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Invalid bind address %s:%d"), *BindHost, BindPort);
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("Invalid bind address %s:%d."), *BindHost, BindPort));
	}

	ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_LISTEN"), BindAddr->GetProtocolType());
	if (!ListenSocket)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Failed to create listen socket."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("Failed to create the TCP listen socket."));
	}

	ListenSocket->SetReuseAddr(true);
	ListenSocket->SetNonBlocking(true);

	if (!ListenSocket->Bind(*BindAddr))
	{
		const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Bind failed on %s"), *BindAddr->ToString(true));
		const FString Message = FString::Printf(TEXT("Bind failed on %s (socket error %d)."), *BindAddr->ToString(true), static_cast<int32>(Error));
		DestroySocket();
		return FO3DTransportResult::Error(Error == SE_EADDRINUSE ? EO3DTransportError::AddressInUse : EO3DTransportError::ConnectFailed, Message);
	}

	if (!ListenSocket->Listen(8))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Listen failed on %s"), *BindAddr->ToString(true));
		const FString Message = FString::Printf(TEXT("Listen failed on %s."), *BindAddr->ToString(true));
		DestroySocket();
		return FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, Message);
	}

	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender listening on %s"), *BindAddr->ToString(true));
	return FO3DTransportResult::Ok();
}

void FO3DSocketsTcpSender::DestroySocket()
{
	// Called only while the worker is not running (Start/Stop join it first), so the sockets
	// have no other user.
	check(WorkerThread == nullptr);

	if (ClientSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
	}
	ClientSocket = nullptr;
	PublishState->bClientConnected.store(false);

	if (ListenSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ListenSocket);
	}
	ListenSocket = nullptr;
}

bool FO3DSocketsTcpSender::TryAcceptClient()
{
	if (!ListenSocket || !SocketSubsystem)
	{
		return false;
	}

	TSharedRef<FInternetAddr> PeerAddr = SocketSubsystem->CreateInternetAddr();
	FSocket* Accepted = ListenSocket->Accept(*PeerAddr, TEXT("O3DS_TCP_CLIENT"));
	if (!Accepted)
	{
		return false;
	}

	Accepted->SetNonBlocking(true);

	// Configure socket buffers for better performance
	int32 SendBufferSize = 2 * 1024 * 1024; // 2MB (match UDP)
	int32 AppliedSize = 0;
	Accepted->SetSendBufferSize(SendBufferSize, AppliedSize);

	// Disable Nagle's algorithm for low-latency transmission (critical for audio)
	Accepted->SetNoDelay(true);

	ClientSocket = Accepted;
	PublishState->bClientConnected.store(true); // Fast check in Send() and audio sinks
	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender accepted client %s (sendBuf=%d, TCP_NODELAY=true)"), *PeerAddr->ToString(true), AppliedSize);
	ConnectionState.Set(EO3DConnectionState::Connected); // on the worker thread (ADR 0007 item 3)
	return true;
}

void FO3DSocketsTcpSender::DropClient(const TCHAR* Reason)
{
	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender dropping client: %s"), Reason);
	if (ClientSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
	}
	ClientSocket = nullptr;
	PublishState->bClientConnected.store(false);
	// Still listening: the next receiver that connects is accepted (worker thread).
	ConnectionState.Set(EO3DConnectionState::Reconnecting,
		FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, FString::Printf(TEXT("Receiver dropped: %s."), Reason)));
}

bool FO3DSocketsTcpSender::IsPeerClosed()
{
	// The receiver never sends, so a readable client socket means the peer closed or reset
	// the connection (or sent something we ignore). Without this, a dead client is noticed
	// only when a send fails, and a reconnecting receiver waits in the backlog (TRB-6).
	if (!ClientSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
	{
		return false;
	}

	constexpr int32 ScratchBytes = 256;
	uint8 Scratch[ScratchBytes];
	int32 Read = 0;
	if (ClientSocket->Recv(Scratch, ScratchBytes, Read) && Read > 0)
	{
		return false; // Unexpected data from the receiver; discarded.
	}
	return true;
}

void FO3DSocketsTcpSender::AddDroppedFrames(int64 Count)
{
	if (Count > 0)
	{
		FScopeLock StatsLock(&StatsMutex);
		Stats.DroppedFrames += Count;
	}
}

void FO3DSocketsTcpSender::DropQueuedWithoutClient()
{
	TArray<uint8> Discard;
	int64 Dropped = 0;
	while (PublishState->SendQueue.Dequeue(Discard))
	{
		Dropped += IsControlItem(Discard) ? 0 : 1;
	}
	AddDroppedFrames(Dropped);
}

bool FO3DSocketsTcpSender::DequeueNextFrame(TArray<uint8>& OutItem, int32& OutOffset, double Now)
{
	int64 Expired = 0;
	bool bFound = false;
	while (PublishState->SendQueue.Dequeue(OutItem))
	{
		// TRB-14: a frame that waited longer than the age limit is stale for realtime use.
		// It is dropped whole before any byte of it is sent.
		if (MaxQueueAgeSeconds > 0.0 && (Now - ReadEnqueueTime(OutItem)) > MaxQueueAgeSeconds)
		{
			Expired += IsControlItem(OutItem) ? 0 : 1;
			continue;
		}
		OutOffset = QueueItemPrefixBytes;
		bFound = true;
		break;
	}

	if (Expired > 0)
	{
		AddDroppedFrames(Expired);
		UE_LOG(LogSocketsTcpSender, Verbose, TEXT("TCP sender dropped %lld frames older than %.0f ms"), Expired, MaxQueueAgeSeconds * 1000.0);
	}
	if (!bFound)
	{
		OutItem.Reset();
	}
	return bFound;
}

TSharedPtr<FInternetAddr> FO3DSocketsTcpSender::CreateBindAddress(const FString& Host, int32 Port, bool& bOutValid)
{
	bOutValid = false;
	if (!SocketSubsystem)
	{
		return nullptr;
	}

	TSharedPtr<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	if (!Addr.IsValid())
	{
		return nullptr;
	}

	FString HostToUse = Host;
	if (HostToUse.IsEmpty() || HostToUse == TEXT("*") || HostToUse == TEXT("0.0.0.0"))
	{
		HostToUse = TEXT("0.0.0.0");
	}

	Addr->SetPort(Port);
	Addr->SetIp(*HostToUse, bOutValid);
	return Addr;
}

bool FO3DSocketsTcpSender::StartWorker()
{
	if (!WorkerThread)
	{
		Worker = new FTcpSenderRunnable(*this);
		WorkerThread = FRunnableThread::Create(Worker, TEXT("O3D_TCP_Sender_Worker"));
		if (!WorkerThread)
		{
			delete Worker;
			Worker = nullptr;
			return false;
		}
	}
	return true;
}

void FO3DSocketsTcpSender::StopWorker()
{
	if (WorkerThread)
	{
		WorkerThread->WaitForCompletion();
		delete WorkerThread;
		WorkerThread = nullptr;
	}
	if (Worker)
	{
		delete Worker;
		Worker = nullptr;
	}

	bStopWorker = false;
}

uint32 FO3DSocketsTcpSender::RunWorker()
{
	// The frame being written. Once its first byte is on the wire it is either finished or
	// the client is dropped, so the receiver never sees half a frame followed by another
	// frame (TRB-2).
	TArray<uint8> Pending;
	int32 PendingOffset = 0;
	bool bPendingIsKeepalive = false;

	double LastSendTime = FPlatformTime::Seconds();
	double LastProgressTime = LastSendTime;
	double LastPeerCheckTime = LastSendTime;

	auto ResetPending = [&Pending, &PendingOffset, &bPendingIsKeepalive]()
	{
		Pending.Reset();
		PendingOffset = 0;
		bPendingIsKeepalive = false;
	};

	auto DropClientAndPending = [this, &Pending, &bPendingIsKeepalive, &ResetPending](const TCHAR* Reason)
	{
		if (Pending.Num() > 0 && !bPendingIsKeepalive)
		{
			AddDroppedFrames(1);
		}
		ResetPending();
		DropClient(Reason);
	};

	while (!bStopWorker.Load())
	{
		double Now = FPlatformTime::Seconds();

		if (!ClientSocket)
		{
			// Frames queued just before the client went away have nowhere to go.
			DropQueuedWithoutClient();
			if (!TryAcceptClient())
			{
				PublishState->SendQueue.WaitForWork(AcceptPollMs);
				continue;
			}
			LastSendTime = LastProgressTime = LastPeerCheckTime = FPlatformTime::Seconds();
			continue;
		}

		if (Pending.Num() == 0)
		{
			if (DequeueNextFrame(Pending, PendingOffset, Now))
			{
				bPendingIsKeepalive = false;
			}
			else if (KeepaliveIntervalSeconds > 0.0 && (Now - LastSendTime) >= KeepaliveIntervalSeconds)
			{
				Pending = KeepaliveFrame;
				PendingOffset = 0;
				bPendingIsKeepalive = true;
			}
			else
			{
				if ((Now - LastPeerCheckTime) >= PeerCheckIntervalSeconds)
				{
					LastPeerCheckTime = Now;
					if (IsPeerClosed())
					{
						DropClientAndPending(TEXT("closed by receiver"));
						continue;
					}
				}

				uint32 WaitMs = MaxIdleWaitMs;
				if (KeepaliveIntervalSeconds > 0.0)
				{
					const double UntilKeepalive = KeepaliveIntervalSeconds - (Now - LastSendTime);
					WaitMs = static_cast<uint32>(FMath::Clamp(UntilKeepalive * 1000.0, 1.0, static_cast<double>(MaxIdleWaitMs)));
				}
				PublishState->SendQueue.WaitForWork(WaitMs);
				continue;
			}
			LastProgressTime = Now;
		}

		const int32 Remaining = Pending.Num() - PendingOffset;
		int32 Sent = 0;
		const bool bOk = ClientSocket->Send(Pending.GetData() + PendingOffset, Remaining, Sent);
		Now = FPlatformTime::Seconds();
		if (bOk && Sent > 0)
		{
			PendingOffset += Sent;
			LastProgressTime = Now;
			if (PendingOffset >= Pending.Num())
			{
				ResetPending();
				LastSendTime = Now;
				continue;
			}
		}
		else if (!bOk)
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error != SE_EWOULDBLOCK && Error != SE_ENOBUFS)
			{
				DropClientAndPending(TEXT("send failed"));
				continue;
			}
		}

		// Partial send or EWOULDBLOCK: the kernel send buffer is full because the receiver
		// reads slower than we write. Wait for space, and give up on the client only if the
		// frame makes no progress for tcp.stalltimeout (TRB-2).
		SendWaitCount.fetch_add(1);
		if ((Now - LastProgressTime) > StallTimeoutSeconds)
		{
			DropClientAndPending(TEXT("send stalled"));
			continue;
		}
		ClientSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(WriteWaitSeconds));
	}

	return 0;
}

bool FO3DSocketsTcpSender::EnqueuePayload(const uint8* Data, int32 Size)
{
	if (Size <= 0 || Data == nullptr)
	{
		return false;
	}

	// Framed message (time prefix + header + payload). The shared queue enforces the byte cap
	// atomically and wakes the worker (TRB-3). Over the cap, the new frame is dropped whole.
	return PublishState->SendQueue.Enqueue(MakeQueuedFrame(Data, Size));
}

void FO3DSocketsTcpSender::DrainQueue()
{
	PublishState->SendQueue.Empty();
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
