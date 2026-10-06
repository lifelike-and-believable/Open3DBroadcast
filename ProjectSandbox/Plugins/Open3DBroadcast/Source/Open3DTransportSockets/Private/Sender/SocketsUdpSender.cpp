// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsUdpSender.h"
#include "O3DPerformanceMetrics.h"

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
#include "o3ds/udp_fragment.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogSocketsUdpSender, Log, All);

namespace O3DSocketsUdpSenderPrivate
{
	/** Idle wait of the worker; an Enqueue wakes it earlier. Short, so Stop() never waits long. */
	constexpr uint32 IdleWaitMs = 50;
	constexpr uint32 PausedWaitMs = 5;
	/** Bytes in front of each fragment (core udp_fragment header, v2 since ADR 0009 item 5). */
	constexpr int32 FragmentHeaderSize = static_cast<int32>(kUdpFragmentHeaderSize);

	/** "" and "*" send to the IPv4 broadcast address; "localhost" means 127.0.0.1 (as before WP-A1 PR 4c). */
	FO3DHostPort ApplyHostRules(const FO3DHostPort& In, bool& bOutBroadcast)
	{
		FO3DHostPort Out = In;
		bOutBroadcast = false;
		if (Out.Host.IsEmpty() || Out.Host == TEXT("*"))
		{
			Out.Host = TEXT("255.255.255.255");
			Out.bIPv6 = false;
			bOutBroadcast = true;
		}
		else if (Out.Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
		{
			Out.Host = TEXT("127.0.0.1");
			Out.bIPv6 = false;
		}
		return Out;
	}
}

FO3DSocketsUdpSender::FO3DSocketsUdpSender()
	: Queue(MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>())
	, PublishState(MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue, EO3DAudioWireFormat::UnifiedEnvelope))
{
	// UDP is unreliable and fresh frames beat complete ones (ADR 0008 driver 4): drop the oldest
	// frames while the worker is behind, refuse callers only at twice the soft cap. Audio and
	// control have budgets of their own (ADR 0007 item 7, ADR 0011). No age limit: the drop policy
	// already keeps the backlog at a few frames.
	FO3DSendQueueLimits Limits;
	Limits.Mocap.MaxItems = FrameQueueSoftCap;
	Limits.Mocap.MaxBytes = FrameQueueSoftBytes;
	Limits.MocapOverflow = EO3DMocapOverflow::DropOldest;
	Limits.Audio.MaxBytes = AudioQueueBytes;
	Queue->SetLimits(Limits);
	// No socket yet: sinks refuse PCM until there is one (as the WP-S5 sink did).
	PublishState->SetPeerReady(false);
}

FO3DSocketsUdpSender::~FO3DSocketsUdpSender()
{
	// Stop() closes the audio gate first (waiting for in-flight submits), then joins the
	// worker, then destroys the socket.
	Stop();
}

FO3DTransportResult FO3DSocketsUdpSender::Initialize(const FO3DTransportConfig& Config)
{
	Stop();

	ActiveConfig = Config;
	SenderMetrics = Config.SenderMetrics;
	FramesSent.store(0);
	BytesSent.store(0);
	DroppedFrames.store(0);
	SendErrors.store(0);
	MocapDrained.store(0);
	MocapDroppedBaseline = Queue->GetStats().Mocap.Dropped;
	Endpoint = FO3DHostPort();
	RemoteAddr.Reset();
	StreamId = ActiveConfig.StreamId;
	PublishState->GetSubjectSlot().Reset();

	ActiveAudioConfig = Config.Audio;
	AudioSourceGuid = FGuid::NewGuid();

	FO3DHostPort Parsed;
	if (!O3DSockets::ParseEndpoint(Config, TEXT("udp"), Parsed))
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender requires udp://host:port URI or explicit host/port options."));
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, TEXT("UDP sender requires a udp://host:port URI or explicit host/port options."));
	}

	if (StreamId.IsEmpty())
	{
		StreamId = O3DSockets::ComposeStreamId(Parsed.Host, Parsed.Port);
		ActiveConfig.StreamId = StreamId;
	}

	const TMap<FString, FString>& Options = Config.AdvancedParams;
	bool bBroadcastHost = false;
	Endpoint = O3DSocketsUdpSenderPrivate::ApplyHostRules(Parsed, bBroadcastHost);
	bAllowBroadcast = bBroadcastHost || O3DTransportOptions::GetBool(Options, O3DSockets::BroadcastOptionKey, false);
	MaxDatagramBytes = O3DTransportOptions::GetInt(Options, O3DSockets::MaxDatagramOptionKey, 64000, 512, 65507);
	MtuBytes = FMath::Clamp(O3DTransportOptions::GetInt(Options, O3DSockets::MtuOptionKey, 1200), 256, MaxDatagramBytes);
	FragmentScratch.clear();
	FragmentScratch.reserve(MaxDatagramBytes);

	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender could not access socket subsystem."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("UDP sender could not access the socket subsystem."));
	}

	// An IP literal resolves here without DNS; a host name resolves on the worker (TRB-26).
	if (O3DTransportOptions::IsIpLiteral(Endpoint))
	{
		FString Error;
		if (!O3DTransportOptions::ResolveHostPort(Endpoint, RemoteAddr, &Error) || !RemoteAddr.IsValid())
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender invalid host '%s'."), *Endpoint.Host);
			RemoteAddr.Reset();
			Endpoint = FO3DHostPort();
			return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("UDP sender: invalid host '%s'."), *Parsed.Host));
		}
	}

	PublishState->Open();
	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DSocketsUdpSender::Start()
{
	bRunning.store(false);
	Worker.Stop();
	DestroySocket();
	DrainQueue();

	if (Endpoint.Port <= 0)
	{
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("UDP sender Start() before a successful Initialize()."));
	}
	if (!SocketSubsystem)
	{
		SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	}

	PublishState->Open();
	if (RemoteAddr.IsValid())
	{
		const FO3DTransportResult Result = OpenSocket(RemoteAddr);
		if (!Result.IsOk())
		{
			ConnectionState.End(EO3DConnectionState::Failed, Result);
			return Result;
		}
		// The socket exists: datagrams can go out at once; UDP has no connection.
		ConnectionState.Begin(EO3DConnectionState::Connected);
	}
	else
	{
		// A host name: the worker resolves it, then opens the socket and reports Connected.
		ConnectionState.Begin(EO3DConnectionState::Connecting);
	}

	ResolveBackoff = FO3DReconnectPolicy();
	if (!Worker.Start(TEXT("O3D_UDP_Sender_Worker"), [this]() { return RunWorkerIteration(); }, Queue))
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP sender could not start its worker thread."));
		DestroySocket();
		const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("UDP sender could not start its worker thread."));
		ConnectionState.End(EO3DConnectionState::Failed, Result);
		return Result;
	}
	bRunning.store(true);
	return FO3DTransportResult::Ok();
}

void FO3DSocketsUdpSender::Stop()
{
	// WP-S5 ordering: close the audio gate (waits for in-flight submits), join the worker,
	// destroy the socket, then drop anything still queued.
	bRunning.store(false);
	PublishState->Close();
	Worker.Stop();
	DestroySocket();
	DrainQueue();
	ConnectionState.End(EO3DConnectionState::Idle);
}

void FO3DSocketsUdpSender::DrainQueue()
{
	// Worker joined, so this thread is the queue's only consumer. Empty() returns items of every
	// kind; only the frames it discards are excluded from DroppedFrames.
	const int64 DroppedBefore = Queue->GetStats().Mocap.Dropped;
	Queue->Empty();
	MocapDrained.fetch_add(Queue->GetStats().Mocap.Dropped - DroppedBefore);
}

EO3DSendResult FO3DSocketsUdpSender::EnqueueFrame(FO3DSendItem&& Item)
{
	const EO3DSendResult Result = Queue->Enqueue(MoveTemp(Item));
	if (Result == EO3DSendResult::DroppedBackpressure)
	{
		DroppedFrames.fetch_add(1);
	}
	return Result;
}

EO3DSendResult FO3DSocketsUdpSender::SendSerialized(FO3DSendPayload&& Payload)
{
	if (!bRunning.load())
	{
		return EO3DSendResult::NotRunning;
	}
	if (Payload.Bytes.Num() <= 0)
	{
		return EO3DSendResult::Invalid;
	}

	// Audio frames carry the subject last sent (their metadata's SubjectName).
	if (!Payload.Subject.IsEmpty())
	{
		PublishState->GetSubjectSlot().Set(Payload.Subject);
	}

	// Only enqueued: the worker sends it (TRB-20), fragmenting above udp.maxdatagram.
	return EnqueueFrame(FO3DSendItem::MakeMocap(MoveTemp(Payload.Bytes), MoveTemp(Payload.Subject), Payload.CaptureTimeSec, Payload.bFullSync));
}

/**
 * Control (ADR 0011): one datagram, never fragmented. A control envelope is at most 1,100 bytes
 * (ADR 0011 item 4); if udp.maxdatagram is configured below that, control is refused (TooLarge)
 * rather than fragmented. Queued as a control item with a cap of its own and sent by the worker.
 * Not counted as a frame. UDP is unreliable: the control publisher sends events redundantly and
 * repairs values with snapshots.
 */
EO3DSendResult FO3DSocketsUdpSender::SendControl(const uint8* Envelope, int32 Len)
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
	if (Len > MaxDatagramBytes)
	{
		return EO3DSendResult::TooLarge;
	}
	return Queue->Enqueue(FO3DSendItem::MakeControl(TArray<uint8>(Envelope, Len)));
}

void FO3DSocketsUdpSender::Tick(float /*DeltaSeconds*/)
{
	// Everything runs on the worker.
}

FO3DTransportStats FO3DSocketsUdpSender::GetStats() const
{
	const FO3DSendQueueStats QueueStats = Queue->GetStats();
	FO3DTransportStats Copy;
	Copy.FramesSent = FramesSent.load();
	Copy.BytesSent = BytesSent.load();
	// Frames refused or not sent, plus the oldest frames the queue dropped (not the drains of Stop and Start).
	Copy.DroppedFrames = DroppedFrames.load() + FMath::Max<int64>(0, QueueStats.Mocap.Dropped - MocapDroppedBaseline - MocapDrained.load());
	Copy.SendErrors = SendErrors.load();
	Copy.PendingFrames = QueueStats.Mocap.PendingItems;
	Copy.PendingBytes = QueueStats.GetPendingBytes();
	Copy.State = ConnectionState.Get();
	return Copy;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DSocketsUdpSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
	FO3DTransportAudioConfig EffectiveConfig = ActiveAudioConfig;
	if (AudioConfig.bEnableAudio)
	{
		EffectiveConfig = AudioConfig;
	}
	EffectiveConfig.bEnableAudio = true;
	ActiveAudioConfig = EffectiveConfig;

	// Immutable snapshot for this sink's own encoders (TRB-11): nothing reconfigures them later.
	const FString StreamFallback = ActiveConfig.StreamId.IsEmpty() ? StreamId : ActiveConfig.StreamId;
	FO3DSinkAudioEncoder::FSettings EncoderSettings;
	EncoderSettings.Config = ActiveAudioConfig;
	EncoderSettings.DefaultStreamLabel = StreamFallback;
	EncoderSettings.DefaultSubject = StreamFallback;
	EncoderSettings.SourceGuid = AudioSourceGuid;

	// The shared sink refuses PCM while there is no socket (the publish state's peer flag).
	return MakeShared<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe>(PublishState, ActiveAudioConfig, MoveTemp(EncoderSettings));
}

FO3DTransportResult FO3DSocketsUdpSender::OpenSocket(const TSharedPtr<FInternetAddr>& Addr)
{
	if (!SocketSubsystem || !Addr.IsValid())
	{
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("UDP sender Start() before a successful Initialize()."));
	}

	Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("O3DS_UDP_SENDER"), Addr->GetProtocolType());
	if (!Socket)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("Failed to create UDP socket."));
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("Failed to create the UDP socket."));
	}

	Socket->SetReuseAddr(true);
	Socket->SetNonBlocking(true);
	if (bAllowBroadcast)
	{
		Socket->SetBroadcast(true);
	}

	int32 RequestedSize = 2 * 1024 * 1024;
	int32 AppliedSize = 0;
	Socket->SetSendBufferSize(RequestedSize, AppliedSize);

	UE_LOG(LogSocketsUdpSender, Log, TEXT("UDP sender targeting %s:%d (broadcast=%d, maxDatagram=%d, mtu=%d, sendBuf=%d)."),
		*Addr->ToString(false), Addr->GetPort(), bAllowBroadcast ? 1 : 0, MaxDatagramBytes, MtuBytes, AppliedSize);

	PublishState->SetPeerReady(true);
	return FO3DTransportResult::Ok();
}

void FO3DSocketsUdpSender::DestroySocket()
{
	// Only while the worker is not running (Start/Stop join it first), so nobody else uses the socket.
	check(!Worker.IsRunning());
	PublishState->SetPeerReady(false);
	if (Socket && SocketSubsystem)
	{
		SocketSubsystem->DestroySocket(Socket);
	}
	Socket = nullptr;
}

uint32 FO3DSocketsUdpSender::RunWorkerIteration()
{
	using namespace O3DSocketsUdpSenderPrivate;

	if (bWorkerPausedForTesting.load())
	{
		return PausedWaitMs;
	}

	if (!Socket)
	{
		// A host name: resolve it here, off the game thread, retrying with backoff (TRB-26).
		const double Now = FPlatformTime::Seconds();
		if (!ResolveBackoff.IsDue(Now))
		{
			return IdleWaitMs;
		}
		TSharedPtr<FInternetAddr> Addr;
		FString Error;
		if (!O3DTransportOptions::ResolveHostPort(Endpoint, Addr, &Error) || !Addr.IsValid())
		{
			UE_LOG(LogSocketsUdpSender, Verbose, TEXT("UDP sender could not resolve %s: %s"), *Endpoint.ToString(), *Error);
			ResolveBackoff.OnFailure(Now);
			return IdleWaitMs;
		}
		if (!OpenSocket(Addr).IsOk())
		{
			ResolveBackoff.OnFailure(Now);
			return IdleWaitMs;
		}
		RemoteAddr = Addr;
		ResolveBackoff.OnSuccess();
		ConnectionState.Set(EO3DConnectionState::Connected); // on the worker thread (ADR 0007 item 3)
		return 0;
	}

	FO3DSendItem Item;
	// DropOldest: while more than FrameQueueSoftCap frames wait, Dequeue discards the oldest.
	const bool bDequeued = Queue->Dequeue(Item);
	for (int32 Discarded = Queue->ConsumeMocapDiscarded(); Discarded > 0 && SenderMetrics.IsValid(); --Discarded)
	{
		SenderMetrics->RecordTransportFrameDropped(); // WP-R3
	}
	if (!bDequeued)
	{
		return IdleWaitMs;
	}

	switch (Item.Kind)
	{
	case EO3DSendItemKind::Mocap:
		if (SendPayload(Item.Bytes.GetData(), Item.Bytes.Num(), TEXT("data")))
		{
			FramesSent.fetch_add(1);
			BytesSent.fetch_add(Item.Bytes.Num());
			if (SenderMetrics.IsValid())
			{
				SenderMetrics->RecordBytesSent(static_cast<uint64>(Item.Bytes.Num())); // WP-R3
			}
		}
		else
		{
			// The socket refused the datagram (send buffer full or a network error).
			DroppedFrames.fetch_add(1);
			SendErrors.fetch_add(1);
			if (SenderMetrics.IsValid())
			{
				SenderMetrics->RecordTransportFrameDropped(); // WP-R3
			}
		}
		break;
	case EO3DSendItemKind::Audio:
		if (SendPayload(Item.Bytes.GetData(), Item.Bytes.Num(), TEXT("audio")))
		{
			BytesSent.fetch_add(Item.Bytes.Num());
		}
		else
		{
			SendErrors.fetch_add(1);
		}
		break;
	case EO3DSendItemKind::Control:
		// Never fragmented: SendControl refused anything above udp.maxdatagram.
		if (!SendDatagram(Item.Bytes.GetData(), Item.Bytes.Num(), TEXT("control")))
		{
			SendErrors.fetch_add(1);
		}
		break;
	}
	return 0;
}

bool FO3DSocketsUdpSender::SendPayload(const uint8* Data, int32 Size, const TCHAR* Context)
{
	if (Size <= MaxDatagramBytes)
	{
		return SendDatagram(Data, Size, Context);
	}
	return SendFragmented(Data, Size, Context);
}

bool FO3DSocketsUdpSender::SendDatagram(const uint8* Data, int32 Size, const TCHAR* Context)
{
	if (!Socket || !RemoteAddr.IsValid() || Data == nullptr || Size <= 0)
	{
		return false;
	}

	int32 BytesSentNow = 0;
	if (!Socket->SendTo(Data, Size, BytesSentNow, *RemoteAddr))
	{
		const ESocketErrors Error = SocketSubsystem ? SocketSubsystem->GetLastErrorCode() : SE_NO_ERROR;
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP %s send failed (size=%d, error=%d)."), Context, Size, static_cast<int32>(Error));
		return false;
	}
	if (BytesSentNow != Size)
	{
		UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP %s partial send (requested=%d, sent=%d)."), Context, Size, BytesSentNow);
		return false;
	}
	return true;
}

bool FO3DSocketsUdpSender::SendFragmented(const uint8* Data, int32 Size, const TCHAR* Context)
{
	using namespace O3DSocketsUdpSenderPrivate;
	if (Data == nullptr || Size <= 0)
	{
		return false;
	}

	const int32 FragmentPayload = FMath::Clamp(MtuBytes - FragmentHeaderSize, 256, MaxDatagramBytes);
	UdpFragmenter Fragmenter(reinterpret_cast<const char*>(Data), static_cast<size_t>(Size), static_cast<size_t>(FragmentPayload));
	const uint32 MessageId = ++MessageCounter;

	for (uint32 Seq = 0; Seq < static_cast<uint32>(Fragmenter.mFrames); ++Seq)
	{
		FragmentScratch.clear();
		Fragmenter.makeFragment(MessageId, Seq, FragmentScratch);
		if (!SendDatagram(reinterpret_cast<const uint8*>(FragmentScratch.data()), static_cast<int32>(FragmentScratch.size()), Context))
		{
			UE_LOG(LogSocketsUdpSender, Warning, TEXT("UDP %s fragment send failed (seq=%u/%llu)."), Context, Seq, static_cast<unsigned long long>(Fragmenter.mFrames));
			return false;
		}
	}
	return true;
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
