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

#include "o3ds/model.h"

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
	/** Prefix a payload with the TCP frame header. Safe on any thread. */
	TArray<uint8> MakeTcpFrame(const uint8* Data, int32 Size)
	{
		TArray<uint8> Framed;
		const int32 HeaderSize = O3DSockets::Tcp::FrameHeaderSize;
		Framed.SetNumUninitialized(HeaderSize + Size);
		O3DSockets::Tcp::WriteFrameHeader(Framed.GetData(), Size);
		FMemory::Memcpy(Framed.GetData() + HeaderSize, Data, Size);
		return Framed;
	}
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
		if (!State->SendQueue.Enqueue(MakeTcpFrame(Unified.GetData(), Unified.Num())))
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
	: PublishState(MakeShared<FSocketsTcpPublishState, ESPMode::ThreadSafe>())
{
	PublishState->SendQueue.SetMaxBytes(DefaultMaxQueueBytes);
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

	PublishState->Gate->Open();

	return true;
}

bool FO3DSocketsTcpSender::Start()
{
	DestroySocket();

	PublishState->Gate->Open();

	bStopWorker = false;
	StartWorker();

	return CreateListenSocket();
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
	SocketSubsystem = nullptr;
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
	TickAcceptClient();
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
	// Same lock as TickAcceptClient()/RunWorker(); blocks until any in-flight
	// worker-thread send finishes before ClientSocket is torn down.
	FScopeLock Lock(&SocketLock);

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

void FO3DSocketsTcpSender::TickAcceptClient()
{
	if (!ListenSocket)
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastAcceptPollTime < 0.01) // Poll every 10ms
	{
		return;
	}
	LastAcceptPollTime = Now;

	// Same lock as DestroySocket()/RunWorker(); guards the ClientSocket
	// read-then-write below against a concurrent worker-thread send/teardown.
	FScopeLock Lock(&SocketLock);

	if (ClientSocket)
	{
		return; // Already have a client
	}

	TSharedRef<FInternetAddr> PeerAddr = SocketSubsystem->CreateInternetAddr();
	FSocket* Accepted = ListenSocket->Accept(*PeerAddr, TEXT("O3DS_TCP_CLIENT"));
	if (Accepted)
	{
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
	}
}

bool FO3DSocketsTcpSender::SendFramed(FSocket* InSocket, const uint8* Data, int32 Size)
{
	if (!InSocket || !Data || Size <= 0)
	{
		return false;
	}

	// Data already includes frame header (added by EnqueuePayload)
	// Send the complete framed message in a single call
	int32 BytesSent = 0;
	if (!InSocket->Send(Data, Size, BytesSent) || BytesSent != Size)
	{
		return false;
	}

	return true;
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

void FO3DSocketsTcpSender::StartWorker()
{
	if (!WorkerThread)
	{
		Worker = new FTcpSenderRunnable(*this);
		WorkerThread = FRunnableThread::Create(Worker, TEXT("O3D_TCP_Sender_Worker"));
	}
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
	TArray<uint8> Bytes;
	while (!bStopWorker.Load())
	{
		if (!PublishState->SendQueue.Dequeue(Bytes))
		{
			PublishState->SendQueue.WaitForWork(50);
			continue;
		}

		// Same lock as TickAcceptClient()/DestroySocket(); held for the whole
		// send so a concurrent accept/teardown on the game thread can't touch
		// ClientSocket (or free the underlying FSocket) mid-send. The audio
		// thread never takes this lock (WP-S5, TRB-10).
		FScopeLock Lock(&SocketLock);

		FSocket* ActiveSocket = ClientSocket;
		if (!ActiveSocket)
		{
			FScopeLock StatsLock(&StatsMutex);
			Stats.DroppedFrames++;
			continue;
		}

		if (!SendFramed(ActiveSocket, Bytes.GetData(), Bytes.Num()))
		{
			// Send failed - drop client and wait for reconnect
			UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP send failed, dropping client."));
			if (ClientSocket && SocketSubsystem)
			{
				SocketSubsystem->DestroySocket(ClientSocket);
			}
			ClientSocket = nullptr;
			PublishState->bClientConnected.store(false); // Update connection state

			FScopeLock StatsLock(&StatsMutex);
			Stats.DroppedFrames++;
			continue;
		}
	}

	return 0;
}

bool FO3DSocketsTcpSender::EnqueuePayload(const uint8* Data, int32 Size)
{
	if (Size <= 0 || Data == nullptr)
	{
		return false;
	}

	// Framed message (header + payload). The shared queue enforces the byte cap atomically
	// and wakes the worker.
	return PublishState->SendQueue.Enqueue(MakeTcpFrame(Data, Size));
}

void FO3DSocketsTcpSender::DrainQueue()
{
	PublishState->SendQueue.Empty();
}
