// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTcpReceiver.h"
#include "../Shared/SocketsTcpTransport.h"
#include "O3DUnifiedMessage.h"
#include "Transport/O3DTransportTypes.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogSocketsTcpReceiver, Log, All);

namespace O3DSocketsTcpReceiverPrivate
{
	/** Bytes requested from the socket per Recv call. */
	constexpr int32 RecvChunkBytes = 64 * 1024;
	/** Work bounds per worker iteration, so Stop() never waits long for the join. */
	constexpr int32 MaxFramesPerIteration = 256;
	constexpr int64 MaxBytesPerIteration = 8 * 1024 * 1024;
	/** Work bounds per Poll() so a burst cannot hold the game thread (TRB-18); the rest waits in the queue. */
	constexpr int32 MaxItemsPerPoll = 256;
	constexpr int64 MaxBytesPerPoll = 8 * 1024 * 1024;
	/** Worker waits, short so Stop() returns promptly. */
	constexpr double ReadWaitSeconds = 0.010;
	constexpr uint32 ConnectPollMs = 5;
	constexpr uint32 FullQueueRetryMs = 2;
	constexpr uint32 MaxIdleWaitMs = 50;

	/** An envelope with no payload: the sender's keepalive (TRB-6). It only refreshes the idle timer. */
	bool IsKeepalive(const uint8* Data, int32 Size)
	{
		O3DS::FUnifiedHeader Header;
		const uint8* PayloadPtr = nullptr;
		int32 PayloadSize = 0;
		return O3DS::ParseUnifiedMessage(Data, Size, Header, PayloadPtr, PayloadSize) && PayloadSize == 0;
	}
}

FO3DSocketsTcpReceiver::FO3DSocketsTcpReceiver()
	: ReceiveQueue(MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>())
{
}

FO3DSocketsTcpReceiver::~FO3DSocketsTcpReceiver()
{
	Stop();
}

FO3DTransportResult FO3DSocketsTcpReceiver::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	FramesReceived.store(0);
	BytesReceived.store(0);
	DroppedFrames.store(0);
	ReceiveErrors.store(0);
	StreamId = ActiveConfig.StreamId;
	ActiveAudioConfig = Config.Audio;

	if (!O3DSockets::ParseEndpoint(Config, TEXT("tcp"), RemoteEndpoint))
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver requires tcp://host:port URI or explicit host/port options."));
		RemoteEndpoint = FO3DHostPort();
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("TCP receiver requires a tcp://host:port URI or explicit host/port options."));
	}

	if (StreamId.IsEmpty())
	{
		StreamId = RemoteEndpoint.ToString();
		ActiveConfig.StreamId = StreamId;
	}

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver could not access socket subsystem."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("TCP receiver could not access the socket subsystem."));
	}

	namespace Tcp = O3DSockets::Tcp;
	const TMap<FString, FString>& Options = Config.AdvancedParams;

	// Idle timeout: no data (frames or keepalives) for this long forces a reconnect.
	ConnectionTimeoutSeconds = FMath::Max(0.5, static_cast<double>(O3DTransportOptions::GetInt(Options, O3DSockets::TimeoutOptionKey, 5)));
	// TRB-5: a connect that has not completed after this long is abandoned and retried.
	ConnectTimeoutSeconds = FMath::Max(0.5, static_cast<double>(O3DTransportOptions::GetInt(Options, Tcp::ConnectTimeoutOptionKey, Tcp::DefaultConnectTimeoutSeconds)));
	// TRB-4: exponential reconnect backoff with jitter, reset once a connection carries data.
	InitialBackoffSeconds = O3DTransportOptions::GetInt(Options, Tcp::BackoffOptionKey, Tcp::DefaultBackoffMs, 10, 60000) / 1000.0;
	MaxBackoffSeconds = FMath::Max(InitialBackoffSeconds, O3DTransportOptions::GetInt(Options, Tcp::MaxBackoffOptionKey, Tcp::DefaultMaxBackoffMs, 10, 600000) / 1000.0);
	FO3DReconnectPolicySettings BackoffSettings;
	BackoffSettings.InitialDelaySeconds = InitialBackoffSeconds;
	BackoffSettings.MaxDelaySeconds = MaxBackoffSeconds;
	BackoffSettings.Multiplier = 2.0;
	BackoffSettings.JitterFraction = 0.2;
	Backoff = FO3DReconnectPolicy(BackoffSettings);
	// TRB-9: largest accepted frame.
	const int32 MaxFrameBytes = O3DTransportOptions::GetInt(Options, Tcp::MaxFrameOptionKey, Tcp::DefaultMaxFrameBytes, Tcp::MinFrameBytes, Tcp::MaxFrameBytesLimit);
	Parser.setMaxPayloadBytes(static_cast<size_t>(MaxFrameBytes));

	// The hand-off queue always fits one largest frame; beyond its cap the worker stops reading.
	FO3DSendQueueLimits Limits;
	Limits.Mocap.MaxBytes = FMath::Max<int64>(Tcp::DefaultReceiveQueueBytes, MaxFrameBytes);
	Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
	ReceiveQueue->SetLimits(Limits);

	FO3DReceiveDemuxSettings DemuxSettings = Demux.GetSettings();
	DemuxSettings.StreamId = StreamId;
	Demux.SetSettings(DemuxSettings);
	Demux.ResetStats();

	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DSocketsTcpReceiver::Start()
{
	bRunning = false;
	Worker.Stop();
	DisconnectSocket(/*bReportLoss=*/false);
	ReceiveQueue->Empty();

	// TRB-13: Stop() keeps SocketSubsystem, but fetch it again in case Initialize() was skipped.
	if (!SocketSubsystem)
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}
	if (!SocketSubsystem || RemoteEndpoint.Port <= 0)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP receiver cannot start: not initialized."));
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("TCP receiver Start() before a successful Initialize()."));
	}
	if (!Demux.HasConsumer())
	{
		return FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("TCP receiver Start() without a frame consumer (SetConsumer)."));
	}

	ConnectCount.store(0);
	FailedConnectAttempts.store(0);
	Backoff.Reset();
	PendingHandOff.Reset();
	// Connecting until the worker has a connection; before the worker starts, so its changes follow.
	ConnectionState.Begin(EO3DConnectionState::Connecting);
	bRunning = true;
	if (!Worker.Start(TEXT("O3D_TCP_Receiver_Worker"), [this]() { return RunWorkerIteration(); }))
	{
		bRunning = false;
		const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("TCP receiver could not start its worker thread."));
		ConnectionState.End(EO3DConnectionState::Failed, Result);
		return Result;
	}
	UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connecting to %s"), *RemoteEndpoint.ToString());
	return FO3DTransportResult::Ok();
}

void FO3DSocketsTcpReceiver::Stop()
{
	bRunning = false;
	Worker.Stop();
	DisconnectSocket(/*bReportLoss=*/false);
	ReceiveQueue->Empty();
	PendingHandOff.Reset();
	Demux.ReleaseSinks();
	ConnectionState.End(EO3DConnectionState::Idle);
}

int32 FO3DSocketsTcpReceiver::Poll()
{
	using namespace O3DSocketsTcpReceiverPrivate;
	if (!bRunning)
	{
		return 0;
	}

	int32 Delivered = 0;
	int32 Items = 0;
	int64 Bytes = 0;
	FO3DSendItem Item;
	while (Items < MaxItemsPerPoll && Bytes < MaxBytesPerPoll && ReceiveQueue->Dequeue(Item))
	{
		++Items;
		Bytes += Item.Bytes.Num();
		const EO3DDemuxResult Result = Demux.ProcessMessage(Item.Bytes.GetData(), Item.Bytes.Num(), FPlatformTime::Seconds());
		switch (Result)
		{
		case EO3DDemuxResult::Mocap:
			FramesReceived.fetch_add(1);
			BytesReceived.fetch_add(Item.Bytes.Num());
			++Delivered;
			break;
		case EO3DDemuxResult::Audio:
			++Delivered;
			break;
		case EO3DDemuxResult::AudioRejected:
			ReceiveErrors.fetch_add(1);
			UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP receiver rejected an audio frame (%d bytes)."), Item.Bytes.Num());
			break;
		case EO3DDemuxResult::Malformed:
		case EO3DDemuxResult::Oversize:
			ReceiveErrors.fetch_add(1);
			break;
		default:
			// Control (to the control sink, not a frame), keepalives and unknown kinds.
			break;
		}
		if (!bRunning)
		{
			break; // A consumer stopped this receiver from inside SubmitFrame.
		}
	}
	return Delivered;
}

FO3DTransportStats FO3DSocketsTcpReceiver::GetStats() const
{
	FO3DTransportStats Copy;
	Copy.FramesReceived = FramesReceived.load();
	Copy.BytesReceived = BytesReceived.load();
	Copy.DroppedFrames = DroppedFrames.load();
	Copy.ReceiveErrors = ReceiveErrors.load();
	Copy.PendingFrames = ReceiveQueue->GetPendingItems(EO3DSendItemKind::Mocap);
	Copy.PendingBytes = ReceiveQueue->GetPendingBytes();
	Copy.State = ConnectionState.Get();
	return Copy;
}

void FO3DSocketsTcpReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
	Demux.SetAudioSink(Sink);
	if (!Sink.IsValid())
	{
		return;
	}

	FO3DTransportAudioConfig EffectiveConfig = ActiveAudioConfig;
	if (AudioConfig.bEnableAudio)
	{
		EffectiveConfig = AudioConfig;
	}
	EffectiveConfig.bEnableAudio = true;
	EffectiveConfig.NumChannels = FMath::Max(EffectiveConfig.NumChannels, 1);
	EffectiveConfig.SampleRate = FMath::Max(EffectiveConfig.SampleRate, 1);
	ActiveAudioConfig = EffectiveConfig;
}

uint32 FO3DSocketsTcpReceiver::RunWorkerIteration()
{
	using namespace O3DSocketsTcpReceiverPrivate;
	const double Now = FPlatformTime::Seconds();

	if (!Socket)
	{
		// Reconnect after an exponentially growing, jittered delay (TRB-4). The first attempt
		// after Start, and the first retry after a connection that carried data, wait the least.
		if (!Backoff.IsDue(Now))
		{
			return MaxIdleWaitMs;
		}
		if (!ConnectToServer())
		{
			NoteConnectFailure(Now);
		}
		return 0;
	}

	if (State == EState::Connecting)
	{
		const ESocketConnectionState ConnState = Socket->GetConnectionState();
		if (ConnState == SCS_Connected)
		{
			State = EState::Connected;
			LastDataReceiveTime = Now;
			UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP receiver connected to %s"), *RemoteEndpoint.ToString());
			// State first, then the flags tests and diagnostics read.
			ConnectionState.Set(EO3DConnectionState::Connected);
			ConnectCount.fetch_add(1);
			bConnected.store(true);
			return 0;
		}
		if (ConnState == SCS_ConnectionError)
		{
			UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP connection error, will retry"));
			DisconnectSocket(/*bReportLoss=*/true);
			NoteConnectFailure(Now);
			return 0;
		}
		if ((Now - ConnectStartTime) > ConnectTimeoutSeconds)
		{
			// TRB-5: a SYN that is never answered (firewall, wrong subnet) must not leave the
			// receiver in Connecting forever.
			UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP connect to %s timed out after %.1fs, will retry"), *RemoteEndpoint.ToString(), Now - ConnectStartTime);
			DisconnectSocket(/*bReportLoss=*/true);
			NoteConnectFailure(Now);
			return 0;
		}
		return ConnectPollMs;
	}

	// Connected.
	if (Socket->GetConnectionState() != SCS_Connected)
	{
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP connection lost, will reconnect"));
		DisconnectSocket(/*bReportLoss=*/true);
		NoteConnectFailure(Now);
		return 0;
	}
	if ((Now - LastDataReceiveTime) > ConnectionTimeoutSeconds)
	{
		// No frames and no keepalives: treat the connection as dead. A sender that is only
		// idle sends keepalives (TRB-6).
		UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP connection timeout (no data for %.1fs), forcing reconnect"), Now - LastDataReceiveTime);
		DisconnectSocket(/*bReportLoss=*/true);
		NoteConnectFailure(Now);
		return 0;
	}

	// A payload the full queue refused goes first; until it fits, nothing more is read, so the
	// kernel buffers fill and TCP slows the sender down.
	if (PendingHandOff.Num() > 0)
	{
		if (!HandOff(PendingHandOff.GetData(), PendingHandOff.Num()))
		{
			LastDataReceiveTime = Now; // Data is waiting for Poll; the connection is not idle.
			return FullQueueRetryMs;
		}
		PendingHandOff.Reset();
	}

	if (!ReadAvailable())
	{
		UE_LOG(LogSocketsTcpReceiver, Log, TEXT("TCP connection to %s closed by peer, will reconnect"), *RemoteEndpoint.ToString());
		ReportParserStats();
		DisconnectSocket(/*bReportLoss=*/true);
		NoteConnectFailure(Now);
		return 0;
	}
	ReportParserStats();
	if (PendingHandOff.Num() > 0)
	{
		return FullQueueRetryMs;
	}
	// Wait for the next bytes (bounded, so Stop joins promptly).
	Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(ReadWaitSeconds));
	return 0;
}

void FO3DSocketsTcpReceiver::NoteConnectFailure(double Now)
{
	Backoff.OnFailure(Now);
	FailedConnectAttempts.store(Backoff.GetFailedAttempts());
}

bool FO3DSocketsTcpReceiver::HandOff(const uint8* Payload, int32 Size)
{
	TArray<uint8> Bytes(Payload, Size);
	return ReceiveQueue->Enqueue(FO3DSendItem::MakeMocap(MoveTemp(Bytes), FString(), FPlatformTime::Seconds())) == EO3DSendResult::Queued;
}

bool FO3DSocketsTcpReceiver::ReadAvailable()
{
	using namespace O3DSocketsTcpReceiverPrivate;
	int32 Frames = 0;
	int64 BytesRead = 0;

	// TRB-1: pop every complete frame already buffered before reading more, and read only
	// when nothing complete is left.
	while (Frames < MaxFramesPerIteration && BytesRead < MaxBytesPerIteration)
	{
		const uint8_t* Payload = nullptr;
		size_t PayloadSize = 0;
		if (Parser.next(Payload, PayloadSize))
		{
			LastDataReceiveTime = FPlatformTime::Seconds();
			if (!bReceivedOnThisConnection)
			{
				// The connection carries data: only now does the backoff start over (TRB-4).
				bReceivedOnThisConnection = true;
				Backoff.OnSuccess();
				FailedConnectAttempts.store(0);
			}
			const int32 Size = static_cast<int32>(PayloadSize);
			if (Size <= 0 || IsKeepalive(Payload, Size))
			{
				continue; // keepalive or empty frame: refreshes the idle timer only
			}
			++Frames;
			if (!HandOff(Payload, Size))
			{
				PendingHandOff = TArray<uint8>(Payload, Size);
				return true;
			}
			continue;
		}

		uint8* Destination = Parser.prepareWrite(RecvChunkBytes);
		int32 Read = 0;
		const bool bOk = Socket->Recv(Destination, RecvChunkBytes, Read);
		if (bOk && Read > 0)
		{
			Parser.commitWrite(static_cast<size_t>(Read));
			BytesRead += Read;
			continue;
		}

		if (!bOk)
		{
			const ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
			{
				return false; // Reset, aborted or another hard error.
			}
		}

		// Nothing read. A stream socket that reports readable but then yields no bytes has
		// reached end of stream (the sender closed or restarted) or failed. The error code is
		// not used here: after an orderly close it can still hold a stale EWOULDBLOCK. The
		// second Recv means a byte arriving between the two calls is not mistaken for a close.
		if (Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
		{
			Read = 0;
			Destination = Parser.prepareWrite(RecvChunkBytes);
			if (Socket->Recv(Destination, RecvChunkBytes, Read) && Read > 0)
			{
				Parser.commitWrite(static_cast<size_t>(Read));
				BytesRead += Read;
				continue;
			}
			return false;
		}
		break;
	}
	return true;
}

void FO3DSocketsTcpReceiver::ReportParserStats()
{
	const O3DS::TcpStreamParserStats& ParserStats = Parser.stats();
	if (ParserStats.rejectedFrames > ReportedRejectedFrames)
	{
		DroppedFrames.fetch_add(static_cast<int64>(ParserStats.rejectedFrames - ReportedRejectedFrames));
		ReportedRejectedFrames = ParserStats.rejectedFrames;
	}

	if (ParserStats.discardedBytes > ReportedDiscardedBytes)
	{
		const uint64 NewBytes = ParserStats.discardedBytes - ReportedDiscardedBytes;
		ReportedDiscardedBytes = ParserStats.discardedBytes;
		if (!bWarnedResyncThisConnection)
		{
			bWarnedResyncThisConnection = true;
			UE_LOG(LogSocketsTcpReceiver, Warning, TEXT("TCP stream from %s lost framing: skipped %llu bytes to the next frame header (rejected frames so far: %llu, tcp.maxframe=%llu). Further resyncs on this connection are logged at Verbose."),
				*RemoteEndpoint.ToString(), static_cast<unsigned long long>(NewBytes), static_cast<unsigned long long>(ParserStats.rejectedFrames), static_cast<unsigned long long>(Parser.maxPayloadBytes()));
		}
		else
		{
			UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP stream resync: skipped %llu bytes"), static_cast<unsigned long long>(NewBytes));
		}
	}
}

bool FO3DSocketsTcpReceiver::ConnectToServer()
{
	if (!SocketSubsystem)
	{
		return false;
	}

	DisconnectSocket(/*bReportLoss=*/false);

	// TRB-26: host names resolve here, on the worker, never on the game thread.
	TSharedPtr<FInternetAddr> Addr;
	FString ResolveError;
	if (!O3DTransportOptions::ResolveHostPort(RemoteEndpoint, Addr, &ResolveError) || !Addr.IsValid())
	{
		UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP receiver could not resolve %s: %s"), *RemoteEndpoint.ToString(), *ResolveError);
		return false;
	}

	// The socket's protocol follows the resolved address, so an IPv6 sender is reachable.
	Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_CLIENT"), Addr->GetProtocolType());
	if (!Socket)
	{
		return false;
	}

	Socket->SetNonBlocking(true);

	// Configure socket buffers for better performance
	int32 ReceiveBufferSize = 2 * 1024 * 1024; // 2MB (match UDP)
	int32 AppliedSize = 0;
	Socket->SetReceiveBufferSize(ReceiveBufferSize, AppliedSize);

	// Disable Nagle's algorithm for low-latency transmission (critical for audio)
	Socket->SetNoDelay(true);

	State = EState::Connecting;
	Socket->Connect(*Addr);
	const double Now = FPlatformTime::Seconds();
	ConnectStartTime = Now;
	LastDataReceiveTime = Now;
	// TRB-4: the backoff is not reset here. It is reset only once a connection delivers data,
	// so an unreachable or immediately-closing sender backs off to the maximum.
	UE_LOG(LogSocketsTcpReceiver, Verbose, TEXT("TCP receiver connect attempt %d to %s (recvBuf=%d, TCP_NODELAY=true)"), Backoff.GetFailedAttempts(), *RemoteEndpoint.ToString(), AppliedSize);
	return true;
}

void FO3DSocketsTcpReceiver::DisconnectSocket(bool bReportLoss)
{
	if (bReportLoss && bRunning && State == EState::Connected)
	{
		// A live connection went away; the worker reconnects with backoff (TRB-4). State first,
		// then the flag tests and diagnostics read.
		ConnectionState.Set(EO3DConnectionState::Reconnecting,
			FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, FString::Printf(TEXT("Connection to %s lost."), *RemoteEndpoint.ToString())));
	}
	bConnected.store(false);
	if (Socket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
	State = EState::Disconnected;
	// A new connection starts on a frame boundary; never splice bytes from the old one.
	Parser.reset();
	bWarnedResyncThisConnection = false;
	bReceivedOnThisConnection = false;
	PendingHandOff.Reset();
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
