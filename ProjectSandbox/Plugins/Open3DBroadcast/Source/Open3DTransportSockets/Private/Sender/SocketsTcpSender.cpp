#include "SocketsTcpSender.h"
#include "../Shared/SocketsTcpAudio.h"
#include "../Shared/SocketsTcpTransport.h"
#include "O3DSenderAudioSinkBase.h"
#include "O3DSinkAudioEncoder.h"
#include "O3DTransportTypes.h"
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

		// Call-local scratch: a sink may be fed from more than one thread (TRF-40).
		TArray<uint8> Unified;
		if (!GetEncoder().EncodeUnified(StreamLabel, FString(), Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Unified))
		{
			return false;
		}

		const int64 Size = Unified.Num();
		if (!State->SendQueue.Enqueue(MakeQueuedFrame(Unified.GetData(), Unified.Num())))
		{
			UE_LOG(LogSocketsTcpSender, Verbose, TEXT("TCP sender failed to enqueue audio frame"));
			return false;
		}
		State->AudioBytesQueued.fetch_add(Size);
		return true;
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

bool FO3DSocketsTcpSender::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	Stats.Reset();
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
		return false;
	}

	if (BindPort <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender requires a valid port (got %d)."), BindPort);
		return false;
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
		return false;
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

	return true;
}

bool FO3DSocketsTcpSender::Start()
{
	// Restart: stop a running worker before touching the sockets it owns.
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
		return false;
	}

	// TRB-13: create the listen socket first, and start the worker only if that worked, so a
	// failed Start() leaves no thread running.
	if (!CreateListenSocket())
	{
		return false;
	}

	PublishState->Gate->Open();

	bStopWorker = false;
	if (!StartWorker())
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender could not start its worker thread."));
		DestroySocket();
		return false;
	}
	return true;
}

void FO3DSocketsTcpSender::Stop()
{
	// WP-S5 ordering: (1) close the audio gate, which waits for in-flight submits;
	// (2) stop and join the worker; (3) destroy sockets; (4) drain. The wake event is owned
	// by the shared queue, so a late Wake() can never hit a pooled event (TRB-12).
	PublishState->Gate->Close();

	bStopWorker = true;
	PublishState->SendQueue.Wake();

	StopWorker();

	DestroySocket();
	DrainQueue();
	// SocketSubsystem is kept so Start() works again without Initialize() (TRB-13).
}

bool FO3DSocketsTcpSender::Send(const O3DS::SubjectList& List)
{
	// Fast path: check connection state without locks
	if (!PublishState->bClientConnected.load())
	{
		// No client connected yet
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

	return SendBytes(reinterpret_cast<const uint8*>(SerializationScratch.data()), BytesWritten);
}

bool FO3DSocketsTcpSender::SendSerialized(const uint8* Data, int32 Len, const FString& /*SubjectName*/, double /*CaptureTimestampSec*/)
{
	if (!PublishState->bClientConnected.load() || Len <= 0)
	{
		return false;
	}

	return SendBytes(Data, Len);
}

/** Enqueue already-serialized bytes for transmission and record transport-level stats. */
bool FO3DSocketsTcpSender::SendBytes(const uint8* Data, int32 Len)
{
	if (!EnqueuePayload(Data, Len))
	{
		FScopeLock Lock(&StatsMutex);
		Stats.DroppedFrames++;
		return false;
	}

	{
		FScopeLock Lock(&StatsMutex);
		Stats.FramesSent++;
		Stats.BytesSent += Len;
	}
	return true;
}

void FO3DSocketsTcpSender::Tick(float /*DeltaSeconds*/)
{
	// Accepting, sending, keepalives and peer-close detection all run on the worker (WP-S6),
	// so the game thread never waits on a socket or on the worker.
}

FO3DTransportStats FO3DSocketsTcpSender::GetStats() const
{
	FScopeLock Lock(&StatsMutex);
	FO3DTransportStats Copy = Stats;
	Copy.BytesSent += PublishState->AudioBytesQueued.load();
	return Copy;
}

bool FO3DSocketsTcpSender::SupportsAudio() const
{
	return true;
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

bool FO3DSocketsTcpSender::CreateListenSocket()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	bool bValid = false;
	TSharedPtr<FInternetAddr> BindAddr = CreateBindAddress(BindHost, BindPort, bValid);
	if (!bValid || !BindAddr.IsValid())
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Invalid bind address %s:%d"), *BindHost, BindPort);
		return false;
	}

	ListenSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_LISTEN"), BindAddr->GetProtocolType());
	if (!ListenSocket)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Failed to create listen socket."));
		return false;
	}

	ListenSocket->SetReuseAddr(true);
	ListenSocket->SetNonBlocking(true);

	if (!ListenSocket->Bind(*BindAddr))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Bind failed on %s"), *BindAddr->ToString(true));
		DestroySocket();
		return false;
	}

	if (!ListenSocket->Listen(8))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Listen failed on %s"), *BindAddr->ToString(true));
		DestroySocket();
		return false;
	}

	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender listening on %s"), *BindAddr->ToString(true));
	return true;
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
		++Dropped;
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
			++Expired;
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
