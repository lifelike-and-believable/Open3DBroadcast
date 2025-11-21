// Copyright (c) Open3DStream Contributors

#include "Sender/QuicSender.h"

#include "Open3DTransportQUICLog.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#include "MoQ/MoQProtocol.h"

#if PLATFORM_WINDOWS
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif
#include "o3ds/model.h"

#include <vector>

namespace
{
	constexpr double NoSubscriberLogIntervalSeconds = 5.0;
}

FO3DQuicSender::FO3DQuicSender() = default;

FO3DQuicSender::~FO3DQuicSender()
{
	Stop();
}

uint32 FO3DQuicSender::FSendWorker::Run()
{
	Owner.RunSendLoop();
	return 0;
}

void FO3DQuicSender::FSendWorker::Stop()
{
	Owner.bStopRequested.store(true);
	if (Owner.WakeEvent)
	{
		Owner.WakeEvent->Trigger();
	}
}

bool FO3DQuicSender::Initialize(const FO3DTransportConfig& Config)
{
	FScopeLock Lock(&Guard);

	if (bInitialized)
	{
		return true;
	}

	FString Error;
	O3DQuic::FQuicSenderOptions Options;
	if (!O3DQuic::ParseSenderOptions(Config, Options, Error))
	{
		LastError = Error;
		UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("QUIC sender failed to parse config: %s"), *Error);
		return false;
	}

	ActiveOptions = Options;

	if (!PublishPrimaryTrack(Error))
	{
		LastError = Error;
		UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("QUIC sender failed to publish track '%s': %s"), *ActiveOptions.TrackName, *Error);
		return false;
	}

	SyncRelayTrackMetadata();

	Stats.Reset();
	bInitialized = true;
	bStarted = false;
	LastError.Reset();

	UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("QUIC sender initialized with %s"), *ActiveOptions.Describe());

	return true;
}

bool FO3DQuicSender::Start()
{
	FScopeLock Lock(&Guard);

	if (!bInitialized)
	{
		UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("QUIC sender Start called before Initialize."));
		return false;
	}

	FString Error;
	if (!EnsureRelayStarted(Error))
	{
		LastError = Error;
		return false;
	}

	if (!StartWorker())
	{
		LastError = TEXT("Failed to start QUIC sender worker thread.");
		return false;
	}

	if (Relay.IsValid())
	{
		Relay->SendAnnounce(CachedAnnouncePayload);
	}

	bStarted = true;
	return true;
}

void FO3DQuicSender::Stop()
{
	StopWorker();

	{
		FScopeLock Lock(&Guard);
		bStarted = false;
		bInitialized = false;
		LastError.Reset();
	}

	if (Relay.IsValid())
	{
		Relay->SendUnannounce(CachedUnannouncePayload);
		Relay->Shutdown();
		Relay.Reset();
	}

	ResetTrackState();
	DrainQueue();
}

bool FO3DQuicSender::Send(const O3DS::SubjectList& List)
{
	if (!bInitialized || !bStarted)
	{
		FScopeLock Lock(&Guard);
		Stats.DroppedFrames++;
		LastError = TEXT("QUIC sender inactive.");
		return false;
	}

	const double TimestampSeconds = FPlatformTime::Seconds();
	TSharedPtr<TArray<uint8>> Payload = SerializeSubjectList(List, TimestampSeconds);
	if (!Payload.IsValid())
	{
		FScopeLock Lock(&Guard);
		Stats.DroppedFrames++;
		LastError = TEXT("Failed to serialize subject list.");
		return false;
	}

	const uint64 TimestampMicros = static_cast<uint64>(TimestampSeconds * 1000000.0);
	if (!EnqueueFrame(Payload, TimestampMicros))
	{
		FScopeLock Lock(&Guard);
		Stats.DroppedFrames++;
		LastError = TEXT("QUIC sender queue overflow.");
		return false;
	}

	return true;
}

void FO3DQuicSender::Tick(float DeltaSeconds)
{
	(void)DeltaSeconds;

	const double Now = FPlatformTime::Seconds();
	if (Now - LastNoSubscriberLogSeconds > NoSubscriberLogIntervalSeconds)
	{
		const bool bHasSubscribers = Relay.IsValid() && Relay->HasActiveSubscribers();
		if (!bHasSubscribers)
		{
			UE_LOG(LogOpen3DTransportQUIC, VeryVerbose, TEXT("QUIC sender awaiting subscribers..."));
		}
		LastNoSubscriberLogSeconds = Now;
	}
}

FO3DTransportStats FO3DQuicSender::GetStats() const
{
	FScopeLock Lock(&Guard);
	return Stats;
}

bool FO3DQuicSender::SupportsAudio() const
{
	return false;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DQuicSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
	(void)AudioConfig;
	return nullptr;
}

bool FO3DQuicSender::PublishPrimaryTrack(FString& OutError)
{
	OutError.Reset();

	if (PrimaryTrackId != 0)
	{
		return true;
	}

	O3DMoQ::FMoQAnnounceMessage Announce;
	const O3DMoQ::FMoQTrackId TrackId = TrackManager.PublishTrack(ActiveOptions.TrackName, ActiveOptions.TrackProperties, Announce, OutError);
	if (TrackId == 0)
	{
		return false;
	}

	FString SerializeError;
	if (!Announce.Serialize(CachedAnnouncePayload, SerializeError))
	{
		OutError = SerializeError;
		return false;
	}

	O3DMoQ::FMoQUnannounceMessage Unannounce;
	Unannounce.TrackId = TrackId;
	Unannounce.Version = O3DMoQ::GetProtocolVersion();
	CachedUnannouncePayload.Reset();
	if (!Unannounce.Serialize(CachedUnannouncePayload))
	{
		OutError = TEXT("Failed to serialize QUIC unannounce message.");
		return false;
	}

	PrimaryTrackId = TrackId;
	return true;
}

bool FO3DQuicSender::EnsureRelayStarted(FString& OutError)
{
	OutError.Reset();

	if (Relay.IsValid())
	{
		return true;
	}

	TSharedPtr<O3DQuic::FLocalQuicPublisherRelay, ESPMode::ThreadSafe> LocalRelay = MakeShared<O3DQuic::FLocalQuicPublisherRelay, ESPMode::ThreadSafe>();
	if (!LocalRelay->Initialize(ActiveOptions, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Failed to initialize local QUIC relay.");
		}
		UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("QUIC sender failed to start relay: %s"), *OutError);
		return false;
	}

	Relay = StaticCastSharedPtr<O3DQuic::IQuicPublisherRelay>(LocalRelay);
	SyncRelayTrackMetadata();
	UE_LOG(LogOpen3DTransportQUIC, Log, TEXT("QUIC sender publishing via local relay at %s:%u"),
		*ActiveOptions.Endpoint.Host,
		ActiveOptions.Endpoint.Port);
	return true;
}

void FO3DQuicSender::SyncRelayTrackMetadata()
{
	if (Relay.IsValid())
	{
		Relay->UpdateTrackMetadata(PrimaryTrackId, CachedAnnouncePayload, CachedUnannouncePayload);
	}
}

void FO3DQuicSender::ResetTrackState()
{
	if (PrimaryTrackId == 0)
	{
		return;
	}

	O3DMoQ::FMoQUnannounceMessage Unannounce;
	if (TrackManager.UnpublishTrack(PrimaryTrackId, Unannounce))
	{
		CachedAnnouncePayload.Reset();
		CachedUnannouncePayload.Reset();
	}

	PrimaryTrackId = 0;
	SyncRelayTrackMetadata();
}

bool FO3DQuicSender::StartWorker()
{
	if (WorkerThread)
	{
		return true;
	}

	if (!WakeEvent)
	{
		WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
	}

	bStopRequested.store(false);
	Worker = MakeUnique<FSendWorker>(*this);
	WorkerThread.Reset(FRunnableThread::Create(Worker.Get(), TEXT("FO3DQuicSenderWorker"), 0, TPri_BelowNormal));
	if (!WorkerThread)
	{
		Worker.Reset();
		return false;
	}

	return true;
}

void FO3DQuicSender::StopWorker()
{
	bStopRequested.store(true);
	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}

	if (WorkerThread)
	{
		WorkerThread->WaitForCompletion();
		WorkerThread.Reset();
	}

	Worker.Reset();

	if (WakeEvent)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

bool FO3DQuicSender::EnqueueFrame(const TSharedPtr<TArray<uint8>>& Payload, uint64 TimestampMicros)
{
	if (!Payload.IsValid())
	{
		return false;
	}

	FScopeLock Lock(&QueueMutex);
	const uint64 FrameBytes = Payload->Num();
	if (FrameBytes == 0 || FrameBytes > MaxQueueBytes)
	{
		return false;
	}

	if (QueuedBytes + FrameBytes > MaxQueueBytes)
	{
		return false;
	}

	FPendingFrame Frame;
	Frame.Payload = Payload;
	Frame.TimestampMicros = TimestampMicros;
	Frame.Sequence = NextSequence++;
	if (NextSequence == 0)
	{
		NextSequence = 1;
	}
	Frame.TrackId = PrimaryTrackId;

	PendingFrames.Enqueue(MoveTemp(Frame));
	QueuedBytes += FrameBytes;

	if (WakeEvent)
	{
		WakeEvent->Trigger();
	}

	return true;
}

bool FO3DQuicSender::DequeueFrame(FPendingFrame& OutFrame)
{
	FScopeLock Lock(&QueueMutex);
	if (!PendingFrames.Dequeue(OutFrame))
	{
		return false;
	}

	if (OutFrame.Payload.IsValid())
	{
		QueuedBytes = (QueuedBytes > static_cast<uint64>(OutFrame.Payload->Num())) ? (QueuedBytes - OutFrame.Payload->Num()) : 0;
	}
	return true;
}

void FO3DQuicSender::DrainQueue()
{
	FScopeLock Lock(&QueueMutex);
	FPendingFrame Frame;
	while (PendingFrames.Dequeue(Frame))
	{
		Frame.Payload.Reset();
	}
	QueuedBytes = 0;
}

void FO3DQuicSender::RunSendLoop()
{
	while (!bStopRequested.load())
	{
		FPendingFrame Frame;
		if (!DequeueFrame(Frame))
		{
			if (WakeEvent)
			{
				WakeEvent->Wait(10);
			}
			continue;
		}

		ProcessFrame(MoveTemp(Frame));
	}
}

void FO3DQuicSender::ProcessFrame(FPendingFrame&& Frame)
{
	if (!Frame.Payload.IsValid() || Frame.Payload->Num() == 0)
	{
		return;
	}

	TArray<uint8> Serialized = BuildObjectMessage(Frame.Payload, Frame.Sequence, Frame.TimestampMicros);
	if (Serialized.Num() == 0)
	{
		return;
	}

	if (!Relay.IsValid())
	{
		FScopeLock StatsLock(&Guard);
		Stats.DroppedFrames++;
		LastError = TEXT("Relay unavailable while sending frame.");
		return;
	}

	FString RelayError;
	const int32 Delivered = Relay->FanoutObject(Serialized, RelayError);
	if (Delivered <= 0)
	{
		FScopeLock StatsLock(&Guard);
		Stats.DroppedFrames++;
		LastError = RelayError;
		return;
	}

	FScopeLock StatsLock(&Guard);
	Stats.FramesSent++;
	Stats.BytesSent += static_cast<int64>(Serialized.Num());
}
TSharedPtr<TArray<uint8>> FO3DQuicSender::SerializeSubjectList(const O3DS::SubjectList& List, double TimestampSeconds) const
{
	std::vector<char> Buffer;
	int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
	if (BytesWritten <= 0)
	{
		return nullptr;
	}

	TSharedPtr<TArray<uint8>> Payload = MakeShared<TArray<uint8>>();
	Payload->SetNum(BytesWritten);
	FMemory::Memcpy(Payload->GetData(), Buffer.data(), BytesWritten);
	return Payload;
}

TArray<uint8> FO3DQuicSender::BuildObjectMessage(const TSharedPtr<TArray<uint8>>& Payload, uint32 Sequence, uint64 TimestampMicros) const
{
	TArray<uint8> Buffer;
	if (!Payload.IsValid())
	{
		return Buffer;
	}

	O3DMoQ::FMoQObjectMessage Message;
	Message.TrackId = PrimaryTrackId;
	Message.Sequence = Sequence;
	Message.TimestampMicros = TimestampMicros;
	Message.Priority = ActiveOptions.TrackProperties.Priority;
	Message.Reliability = ActiveOptions.TrackProperties.Reliability;
	Message.Payload = *Payload;

	FString Error;
	if (!Message.Serialize(Buffer, Error))
	{
		UE_LOG(LogOpen3DTransportQUIC, Warning, TEXT("Failed to serialize MoQ object: %s"), *Error);
		Buffer.Reset();
	}

	return Buffer;
}

#if WITH_DEV_AUTOMATION_TESTS
bool FO3DQuicSender::EnqueueFrameForTesting(const TArray<uint8>& Payload, uint64 TimestampMicros)
{
	TSharedPtr<TArray<uint8>> Buffer = MakeShared<TArray<uint8>>();
	Buffer->Append(Payload);
	return EnqueueFrame(Buffer, TimestampMicros);
}

bool FO3DQuicSender::ProcessSingleFrameForTesting()
{
	FPendingFrame Frame;
	if (!DequeueFrame(Frame))
	{
		return false;
	}

	ProcessFrame(MoveTemp(Frame));
	return true;
}
#endif

