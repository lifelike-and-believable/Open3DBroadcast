// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTcpSender.h"
#include "../Shared/SocketsTcpTransport.h"
#include "O3DSinkAudioEncoder.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DTransportTypes.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpSender, Log, All);

// FO3DSocketsTcpSender::PendingHeader holds 32 bytes.
static_assert(O3DSockets::Tcp::FrameHeaderSize <= 32, "PendingHeader must hold a TCP frame header");

namespace O3DSocketsTcpSenderPrivate
{
	/** Worker waits: short, so Stop() never waits long for the join. */
	constexpr uint32 AcceptPollMs = 10;
	constexpr uint32 MaxIdleWaitMs = 50;
	constexpr double WriteWaitSeconds = 0.010;
	/** How often an idle worker checks whether the receiver closed the connection (TRB-6). */
	constexpr double PeerCheckIntervalSeconds = 0.25;

	/** True for an IPv4 or IPv6 literal or a wildcard: binding to it resolves no name (no DNS on the game thread). */
	bool IsBindableLiteral(const FO3DHostPort& Endpoint)
	{
		return Endpoint.Host.IsEmpty() || Endpoint.Host == TEXT("*") || O3DTransportOptions::IsIpLiteral(Endpoint);
	}
}

FO3DSocketsTcpSender::FO3DSocketsTcpSender()
	: Queue(MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>())
	, PublishState(MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue, EO3DAudioWireFormat::UnifiedEnvelope))
	, KeepalivePayload(O3DSockets::Tcp::MakeKeepalivePayload())
{
	MaxQueueBytes = O3DSockets::Tcp::DefaultMaxQueueBytes;
	MaxQueueAgeSeconds = O3DSockets::Tcp::DefaultMaxQueueAgeMs / 1000.0;
	PublishState->SetPeerReady(false);
	ApplyQueueLimits();
}

FO3DSocketsTcpSender::~FO3DSocketsTcpSender()
{
	// Stop() closes the audio gate first, so every audio-thread submit already inside a sink
	// has returned before the worker and sockets go away.
	Stop();
}

void FO3DSocketsTcpSender::ApplyQueueLimits()
{
	// TRB-14, ADR 0007 item 7. tcp.maxqueue bounds the payload bytes of frames, and separately of
	// audio, so neither takes the other's room; control keeps its own cap. TCP is ReliableOrdered
	// (ADR 0005), so a full queue refuses the newest frame and never discards a queued one; only
	// the age limit (tcp.maxqueueage) discards stale frames and audio before they are sent.
	FO3DSendQueueLimits Limits;
	Limits.Mocap.MaxBytes = MaxQueueBytes;
	Limits.Audio.MaxBytes = MaxQueueBytes;
	Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
	Limits.MaxAgeSeconds = MaxQueueAgeSeconds;
	Queue->SetLimits(Limits);
}

FO3DTransportResult FO3DSocketsTcpSender::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	FramesSent.store(0);
	BytesSent.store(0);
	DroppedFrames.store(0);
	MocapDrained.store(0);
	SendWaitCount.store(0);
	const FO3DSendQueueStats QueueStats = Queue->GetStats();
	MocapDroppedBaseline = QueueStats.Mocap.Dropped;
	AudioBytesBaseline = PublishState->GetAudioBytesQueued();
	StreamId = ActiveConfig.StreamId;

	ActiveAudioConfig = Config.Audio;
	AudioSourceGuid = FGuid::NewGuid();

	if (!O3DSockets::ParseEndpoint(Config, TEXT("tcp"), BindEndpoint))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender requires tcp://host:port URI or explicit host/port options."));
		BindEndpoint = FO3DHostPort();
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("TCP sender requires a tcp://host:port URI or explicit host/port options."));
	}
	if (!O3DSocketsTcpSenderPrivate::IsBindableLiteral(BindEndpoint))
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender bind address '%s' is not an IP address."), *BindEndpoint.Host);
		const FString Message = FString::Printf(TEXT("TCP sender bind address '%s' must be an IP address, 0.0.0.0 or *."), *BindEndpoint.Host);
		BindEndpoint = FO3DHostPort();
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, Message);
	}

	if (StreamId.IsEmpty())
	{
		StreamId = BindEndpoint.ToString();
		ActiveConfig.StreamId = StreamId;
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("TCP sender could not access socket subsystem."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("TCP sender could not access the socket subsystem."));
	}

	namespace Tcp = O3DSockets::Tcp;
	const TMap<FString, FString>& Options = Config.AdvancedParams;

	// TRB-14: configurable queue cap and age limit.
	MaxQueueBytes = FMath::Max(O3DTransportOptions::GetInt(Options, Tcp::MaxQueueOptionKey, Tcp::DefaultMaxQueueBytes), Tcp::MinQueueBytes);
	MaxQueueAgeSeconds = FMath::Max(0, O3DTransportOptions::GetInt(Options, Tcp::MaxQueueAgeOptionKey, Tcp::DefaultMaxQueueAgeMs)) / 1000.0;
	// TRB-2: how long a frame may make no progress before the client is dropped.
	StallTimeoutSeconds = FMath::Max(O3DTransportOptions::GetInt(Options, Tcp::StallTimeoutOptionKey, Tcp::DefaultStallTimeoutMs), Tcp::MinStallTimeoutMs) / 1000.0;
	// TRB-6: keepalive interval while idle.
	KeepaliveIntervalSeconds = FMath::Max(0, O3DTransportOptions::GetInt(Options, Tcp::KeepaliveOptionKey, Tcp::DefaultKeepaliveMs)) / 1000.0;
	ApplyQueueLimits();

	PublishState->Open();
	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DSocketsTcpSender::Start()
{
	// Restart: stop a running worker before touching the sockets it owns.
	bRunning.store(false);
	Worker.Stop();
	DestroySocket();
	MocapDrained.fetch_add(Queue->Empty());

	// TRB-13: Stop() keeps SocketSubsystem; fetch it again only if Initialize() never ran.
	if (!SocketSubsystem)
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}
	if (!SocketSubsystem || BindEndpoint.Port <= 0)
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

	PublishState->Open();

	// Listening, no receiver yet. Before the worker starts, so its "Connected" on accept follows.
	ConnectionState.Begin(EO3DConnectionState::Connecting);

	ResetPending();
	LastSendTime = LastProgressTime = LastPeerCheckTime = FPlatformTime::Seconds();
	if (!Worker.Start(TEXT("O3D_TCP_Sender_Worker"), [this]() { return RunWorkerIteration(); }, Queue))
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
	// WP-S5 ordering: (1) close the audio gate, which waits for in-flight submits; (2) stop and
	// join the worker; (3) destroy sockets; (4) drain. The wake event is owned by the shared
	// queue, so a late wake can never hit a pooled event (TRB-12).
	bRunning.store(false);
	PublishState->Close();

	Worker.Stop();

	DestroySocket();
	MocapDrained.fetch_add(Queue->Empty());
	ResetPending();
	// SocketSubsystem is kept so Start() works again without Initialize() (TRB-13).

	// The worker has been joined, so nothing reports a change after this.
	ConnectionState.End(EO3DConnectionState::Idle);
}

EO3DSendResult FO3DSocketsTcpSender::EnqueueFrame(FO3DSendItem&& Item, int32 Len)
{
	const EO3DSendResult Result = Queue->Enqueue(MoveTemp(Item));
	if (Result == EO3DSendResult::Queued)
	{
		FramesSent.fetch_add(1);
		BytesSent.fetch_add(Len);
	}
	else if (Result == EO3DSendResult::DroppedBackpressure)
	{
		DroppedFrames.fetch_add(1);
	}
	return Result;
}

bool FO3DSocketsTcpSender::Send(const O3DS::SubjectList& List)
{
	if (!bRunning.load() || !PublishState->IsPeerReady())
	{
		return false;
	}

	const double Timestamp = FPlatformTime::Seconds();
	// Not thread-safe (the scratch buffer); Send(SubjectList) is the deprecated game-thread path.
	SerializationScratch.clear();
	const int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(SerializationScratch, Timestamp);
	if (BytesWritten <= 0)
	{
		UE_LOG(LogSocketsTcpSender, Verbose, TEXT("TCP sender failed to serialize SubjectList."));
		DroppedFrames.fetch_add(1);
		return false;
	}

	TArray<uint8> Bytes(reinterpret_cast<const uint8*>(SerializationScratch.data()), BytesWritten);
	return EnqueueFrame(FO3DSendItem::MakeMocap(MoveTemp(Bytes), FString(), Timestamp), BytesWritten) == EO3DSendResult::Queued;
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
	if (!PublishState->IsPeerReady())
	{
		return EO3DSendResult::NotConnected;
	}

	// The payload moves into the queue as it is; the worker writes the frame header (no copy).
	return EnqueueFrame(FO3DSendItem::MakeMocap(MoveTemp(Payload.Bytes), MoveTemp(Payload.Subject), Payload.CaptureTimeSec, Payload.bFullSync), Len);
}

/**
 * Control (ADR 0011): a control item on the shared queue, in order with the frames around it,
 * with a cap of its own (ADR 0007 item 7). Not counted as a frame. Refused without a client
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
	if (!PublishState->IsPeerReady())
	{
		return EO3DSendResult::NotConnected;
	}
	return Queue->Enqueue(FO3DSendItem::MakeControl(TArray<uint8>(Envelope, Len)));
}

void FO3DSocketsTcpSender::Tick(float /*DeltaSeconds*/)
{
	// Accepting, sending, keepalives and peer-close detection all run on the worker (WP-S6),
	// so the game thread never waits on a socket or on the worker.
}

FO3DTransportStats FO3DSocketsTcpSender::GetStats() const
{
	const FO3DSendQueueStats QueueStats = Queue->GetStats();
	FO3DTransportStats Copy;
	Copy.FramesSent = FramesSent.load();
	Copy.BytesSent = BytesSent.load() + (PublishState->GetAudioBytesQueued() - AudioBytesBaseline);
	// Frames the queue discarded (age limit, or queued when the client left), less the drains of Stop and Start.
	Copy.DroppedFrames = DroppedFrames.load() + FMath::Max<int64>(0, QueueStats.Mocap.Dropped - MocapDroppedBaseline - MocapDrained.load());
	Copy.State = ConnectionState.Get();
	Copy.PendingFrames = QueueStats.Mocap.PendingItems;
	Copy.PendingBytes = QueueStats.GetPendingBytes();
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

	ActiveAudioConfig = EffectiveConfig;

	// Immutable snapshot for this sink's own encoders (TRB-11): nothing reconfigures them later.
	const FString StreamFallback = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	FO3DSinkAudioEncoder::FSettings EncoderSettings;
	EncoderSettings.Config = ActiveAudioConfig;
	EncoderSettings.DefaultStreamLabel = StreamFallback;
	EncoderSettings.DefaultSubject = StreamFallback;
	EncoderSettings.SourceGuid = AudioSourceGuid;

	// The shared sink refuses PCM while no receiver is connected (the publish state's peer flag).
	return MakeShared<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe>(PublishState, ActiveAudioConfig, MoveTemp(EncoderSettings));
}

FO3DTransportResult FO3DSocketsTcpSender::CreateListenSocket()
{
	if (!SocketSubsystem)
	{
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("TCP sender has no socket subsystem."));
	}

	// Initialize() accepted only IP literals and wildcards, so this resolves no name (TRB-26).
	TSharedPtr<FInternetAddr> BindAddr;
	FString ResolveError;
	if (!O3DTransportOptions::ResolveHostPort(BindEndpoint, BindAddr, &ResolveError) || !BindAddr.IsValid())
	{
		UE_LOG(LogSocketsTcpSender, Warning, TEXT("Invalid bind address %s: %s"), *BindEndpoint.ToString(), *ResolveError);
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("Invalid bind address %s."), *BindEndpoint.ToString()));
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
	check(!Worker.IsRunning());

	if (ClientSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
	}
	ClientSocket = nullptr;
	PublishState->SetPeerReady(false);

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
	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender accepted client %s (sendBuf=%d, TCP_NODELAY=true)"), *PeerAddr->ToString(true), AppliedSize);
	ConnectionState.Set(EO3DConnectionState::Connected); // on the worker thread (ADR 0007 item 3)
	PublishState->SetPeerReady(true); // Fast check in SendSerialized and the audio sinks
	return true;
}

void FO3DSocketsTcpSender::DropClient(const TCHAR* Reason)
{
	UE_LOG(LogSocketsTcpSender, Log, TEXT("TCP sender dropping client: %s"), Reason);
	PublishState->SetPeerReady(false);
	if (ClientSocket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(ClientSocket);
	}
	ClientSocket = nullptr;
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

void FO3DSocketsTcpSender::DropQueuedWithoutClient()
{
	// Frames queued just before the client went away have nowhere to go.
	FO3DSendItem Discard;
	int64 Dropped = 0;
	while (Queue->Dequeue(Discard))
	{
		Dropped += Discard.Kind == EO3DSendItemKind::Mocap ? 1 : 0;
	}
	DroppedFrames.fetch_add(Dropped);
}

void FO3DSocketsTcpSender::SetPending(TArray<uint8>&& Payload, bool bKeepalive)
{
	PendingPayload = MoveTemp(Payload);
	O3DSockets::Tcp::WriteFrameHeader(PendingHeader, PendingPayload.Num());
	PendingOffset = 0;
	PendingTotal = O3DSockets::Tcp::FrameHeaderSize + PendingPayload.Num();
	bPendingIsKeepalive = bKeepalive;
}

void FO3DSocketsTcpSender::ResetPending()
{
	PendingPayload.Reset();
	PendingOffset = 0;
	PendingTotal = 0;
	bPendingIsKeepalive = false;
	bPendingIsFrame = false;
}

void FO3DSocketsTcpSender::DropClientAndPending(const TCHAR* Reason)
{
	if (PendingTotal > 0 && bPendingIsFrame)
	{
		DroppedFrames.fetch_add(1);
	}
	ResetPending();
	DropClient(Reason);
}

bool FO3DSocketsTcpSender::SendPendingBytes(int32& OutSent)
{
	// Header and payload go out as one frame without being copied into one buffer.
	const int32 HeaderSize = O3DSockets::Tcp::FrameHeaderSize;
	const uint8* Data = nullptr;
	int32 Remaining = 0;
	if (PendingOffset < HeaderSize)
	{
		Data = PendingHeader + PendingOffset;
		Remaining = HeaderSize - PendingOffset;
	}
	else
	{
		Data = PendingPayload.GetData() + (PendingOffset - HeaderSize);
		Remaining = PendingTotal - PendingOffset;
	}
	OutSent = 0;
	return ClientSocket->Send(Data, Remaining, OutSent);
}

uint32 FO3DSocketsTcpSender::RunWorkerIteration()
{
	using namespace O3DSocketsTcpSenderPrivate;
	double Now = FPlatformTime::Seconds();

	if (!ClientSocket)
	{
		DropQueuedWithoutClient();
		if (!TryAcceptClient())
		{
			return AcceptPollMs;
		}
		LastSendTime = LastProgressTime = LastPeerCheckTime = FPlatformTime::Seconds();
		return 0;
	}

	if (PendingTotal == 0)
	{
		FO3DSendItem Item;
		// TRB-14: Dequeue discards frames and audio older than tcp.maxqueueage before any byte of them is sent.
		if (Queue->Dequeue(Item, Now))
		{
			bPendingIsFrame = Item.Kind == EO3DSendItemKind::Mocap;
			SetPending(MoveTemp(Item.Bytes), false);
		}
		else if (KeepaliveIntervalSeconds > 0.0 && (Now - LastSendTime) >= KeepaliveIntervalSeconds)
		{
			TArray<uint8> Keepalive = KeepalivePayload;
			bPendingIsFrame = false;
			SetPending(MoveTemp(Keepalive), true);
		}
		else
		{
			if ((Now - LastPeerCheckTime) >= PeerCheckIntervalSeconds)
			{
				LastPeerCheckTime = Now;
				if (IsPeerClosed())
				{
					DropClientAndPending(TEXT("closed by receiver"));
					return 0;
				}
			}

			uint32 WaitMs = MaxIdleWaitMs;
			if (KeepaliveIntervalSeconds > 0.0)
			{
				const double UntilKeepalive = KeepaliveIntervalSeconds - (Now - LastSendTime);
				WaitMs = static_cast<uint32>(FMath::Clamp(UntilKeepalive * 1000.0, 1.0, static_cast<double>(MaxIdleWaitMs)));
			}
			return WaitMs;
		}
		LastProgressTime = Now;
	}

	int32 Sent = 0;
	const bool bOk = SendPendingBytes(Sent);
	Now = FPlatformTime::Seconds();
	if (bOk && Sent > 0)
	{
		PendingOffset += Sent;
		LastProgressTime = Now;
		if (PendingOffset >= PendingTotal)
		{
			ResetPending();
			LastSendTime = Now;
		}
		return 0;
	}
	if (!bOk)
	{
		const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
		if (Error != SE_EWOULDBLOCK && Error != SE_ENOBUFS)
		{
			DropClientAndPending(TEXT("send failed"));
			return 0;
		}
	}

	// Partial send or EWOULDBLOCK: the kernel send buffer is full because the receiver reads
	// slower than we write. Wait for space, and give up on the client only if the frame makes no
	// progress for tcp.stalltimeout (TRB-2).
	SendWaitCount.fetch_add(1);
	if ((Now - LastProgressTime) > StallTimeoutSeconds)
	{
		DropClientAndPending(TEXT("send stalled"));
		return 0;
	}
	ClientSocket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(WriteWaitSeconds));
	return 0;
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
