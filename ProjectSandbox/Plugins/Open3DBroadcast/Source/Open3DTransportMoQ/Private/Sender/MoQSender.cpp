// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Sender/MoQSender.h"
#include "O3DRedact.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeLock.h"
#include "O3DAudioFrameCodec.h"
#include "O3DPerformanceMetrics.h"
#include "O3DSinkAudioEncoder.h"
#include "O3DUnifiedMessage.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQHelpers.h"
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

DEFINE_LOG_CATEGORY(LogO3DMoQSender);

// Use constants from MoQHelpers
using namespace MoQHelpers;

namespace O3DMoQSenderPrivate
{
	/** Idle wait of the worker; an Enqueue wakes it earlier. */
	constexpr uint32 IdleWaitMs = 100;
	constexpr uint32 PausedWaitMs = 5;

	const TCHAR* TrackLabel(EO3DSendItemKind Kind)
	{
		return Kind == EO3DSendItemKind::Control ? TEXT("control") : (Kind == EO3DSendItemKind::Audio ? TEXT("audio") : TEXT("mocap"));
	}

	/** True for at most one caller per Interval (any thread). */
	bool ClaimLogSlot(std::atomic<double>& Last, double Interval)
	{
		const double Now = FPlatformTime::Seconds();
		double Previous = Last.load();
		return (Now - Previous) >= Interval && Last.compare_exchange_strong(Previous, Now);
	}
}

FO3DMoQSender::FO3DMoQSender()
	// The cycle counter differs per instance and per run, so many senders do not retry in lockstep.
	: FO3DMoQSender(FMoQFfiApi::GetProduction(), nullptr, FPlatformTime::Cycles64())
{
}

FO3DMoQSender::FO3DMoQSender(FMoQFfiApiRef InApi, TFunction<double()> InClock, uint64 InJitterSeed)
	: Api(MoveTemp(InApi))
	, Clock(MoveTemp(InClock))
	, JitterSeed(InJitterSeed)
	, Queue(MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>())
	// The audio track carries bare audio payloads (O3DAudio::SerializeForTransport), not envelopes.
	, PublishState(MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue, EO3DAudioWireFormat::AudioPayload))
	, TransportMetrics(FO3DPerformanceMetrics::Get().AcquireTransportMetrics(TEXT("MoQ")))
{
	CachedState = MOQ_STATE_DISCONNECTED;
}

double FO3DMoQSender::NowSeconds() const
{
	return Clock ? Clock() : FPlatformTime::Seconds();
}

FO3DMoQSender::~FO3DMoQSender()
{
	Stop();
}

FO3DMoQSender::ETrack FO3DMoQSender::TrackOf(EO3DSendItemKind Kind)
{
	return Kind == EO3DSendItemKind::Audio ? ETrack::Audio : (Kind == EO3DSendItemKind::Control ? ETrack::Control : ETrack::Mocap);
}

bool FO3DMoQSender::ParseOptions(const FO3DTransportConfig& Config, FString& OutError)
{
	Options.RelayUrl = ResolveRelayUrl(Config);
	if (Options.RelayUrl.IsEmpty())
	{
		OutError = TEXT("Relay URL is required (set Uri or relay_url advanced parameter)");
		return false;
	}

	// Build separate namespaces for mocap and audio tracks
	Options.MocapNamespace = BuildDefaultMocapNamespace(Config);
	Options.AudioNamespace = BuildDefaultAudioNamespace(Config);
	Options.ControlNamespace = BuildDefaultControlNamespace(Config);
	Options.TrackName = BuildDefaultTrackName(Config);

	if (Options.MocapNamespace.IsEmpty() || Options.TrackName.IsEmpty())
	{
		OutError = TEXT("Unable to derive track namespace/name");
		return false;
	}

	Options.DeliveryMode = ResolveDeliveryMode(Config);
	Options.MaxQueueBytes = ResolveQueueBytes(Config);
	Options.ConnectTimeoutSeconds = ResolveConnectTimeoutSeconds(Config);

	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ sender configured: Relay=%s MocapTrack=%s/%s AudioTrack=%s/%s ControlTrack=%s/%s Mode=%s Queue=%llu bytes ConnectTimeout=%.1fs"),
		*O3DRedact::Url(Options.RelayUrl),
		*Options.MocapNamespace,
		*Options.TrackName,
		*Options.AudioNamespace,
		*Options.TrackName,
		*Options.ControlNamespace,
		*Options.TrackName,
		Options.DeliveryMode == MOQ_DELIVERY_STREAM ? TEXT("stream") : TEXT("datagram"),
		Options.MaxQueueBytes,
		Options.ConnectTimeoutSeconds);

	return true;
}

FO3DTransportResult FO3DMoQSender::Initialize(const FO3DTransportConfig& Config)
{
	if (bRunning)
	{
		UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ sender Initialize called while running"));
		return FO3DTransportResult::Error(EO3DTransportError::Internal, TEXT("MoQ sender Initialize() while running; Stop() it first."));
	}

	FString Error;
	if (!ParseOptions(Config, Error))
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("MoQ sender configuration invalid: %s"), *Error);
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("MoQ sender configuration invalid: %s"), *Error));
	}

	if (!Session.IsValid())
	{
		Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>(Api);
	}

	const FMoQResult InitResult = Session->Initialize(Options.RelayUrl);
	if (!InitResult.IsOk())
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("Failed to initialize MoQ session: %s"), *InitResult.Message);
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, FString::Printf(TEXT("Failed to initialize the MoQ session: %s"), *InitResult.Message));
	}

	if (!ConnectionDelegateHandle.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQSender::HandleConnectionStateChanged);
	}

	ActiveConfig = Config;
	ActiveAudioConfig = Config.Audio;
	AudioSourceGuid = FGuid::NewGuid();
	bAudioRequested = false;

	// queue_bytes is the byte limit of frames; audio and control have budgets of their own, so a
	// full frame budget never refuses them (ADR 0007 item 7, ADR 0011). RefuseNewest: the queue
	// never discards what it accepted (see the class comment).
	FO3DSendQueueLimits Limits;
	Limits.Mocap.MaxBytes = static_cast<int64>(Options.MaxQueueBytes);
	Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
	Limits.Audio.MaxBytes = AudioQueueBytes;
	Queue->SetLimits(Limits);
	// Not running, so no worker consumes the queue. Leftovers of a previous session are not counted.
	Queue->Empty();
	ResetStats();

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	ConsecutiveFailures = 0;
	LastConnectAttemptTimeSeconds = 0.0;
	NextConnectAttemptTimeSeconds = 0.0;
	LastErrorLogTimeSeconds = 0.0;
	LastControlErrorLogTimeSeconds = 0.0;
	LastDropLogTimeSeconds = 0.0;

	PublishState->GetSubjectSlot().Reset();

	bInitialized = true;
	PublishState->Open();
	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DMoQSender::Start()
{
	if (!bInitialized)
	{
		UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ sender Start called before Initialize"));
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("MoQ sender Start() before a successful Initialize()."));
	}

	if (bRunning)
	{
		return FO3DTransportResult::Ok();
	}

	if (!ConnectionDelegateHandle.IsValid() && Session.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQSender::HandleConnectionStateChanged);
	}

	PublishState->Open();

	if (!Worker.Start(TEXT("MoQSenderWorker"), [this]() { return RunWorkerIteration(); }, Queue))
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("Failed to start MoQ sender worker thread"));
		const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, TEXT("Failed to start the MoQ sender worker thread."));
		ConnectionState.End(EO3DConnectionState::Failed, Result);
		return Result;
	}

	bRunning = true;
	// Open the session before connecting: with a fake or fast FFI the CONNECTED state can arrive
	// (on the game thread) before AttemptConnect returns.
	ConnectionState.Begin(EO3DConnectionState::Connecting);
	if (!AttemptConnect())
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("Initial MoQ connection attempt failed"));
		bRunning = false;
		Worker.Stop();
		const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ConnectFailed,
			FString::Printf(TEXT("The first MoQ connection attempt to %s failed."), *O3DRedact::Url(Options.RelayUrl)));
		ConnectionState.End(EO3DConnectionState::Failed, Result);
		return Result;
	}

	return FO3DTransportResult::Ok();
}

void FO3DMoQSender::Stop()
{
	// WP-S5 ordering: close the audio gate first (waits for in-flight submits), then stop the
	// worker, then release publishers and the session.
	PublishState->Close();

	if (!bInitialized && !bRunning)
	{
		ConnectionState.End(EO3DConnectionState::Idle);
		return;
	}

	bRunning = false;

	Worker.Stop();
	DrainQueue();
	DestroyPublisher();
	DestroyAudioPublisher();
	DestroyControlPublisher();

	if (Session.IsValid())
	{
		if (ConnectionDelegateHandle.IsValid())
		{
			Session->OnConnectionStateChanged().Remove(ConnectionDelegateHandle);
			ConnectionDelegateHandle.Reset();
		}
		Session->Disconnect();
	}

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	ConnectionState.End(EO3DConnectionState::Idle);
}

void FO3DMoQSender::ReportSessionLost(const FString& Reason)
{
	// Before the first connection the sender stays Connecting while it retries.
	if (ConnectionState.Get() == EO3DConnectionState::Connected)
	{
		ConnectionState.Set(EO3DConnectionState::Reconnecting, FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, Reason));
	}
}

bool FO3DMoQSender::AttemptConnect()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (bConnectInFlight)
	{
		return true;
	}

	const double Now = NowSeconds();
	bConnectInFlight = true;
	LastConnectAttemptTimeSeconds = Now;

	// Each attempt runs on a fresh client and ends in CONNECTED or FAILED (TRF-11); Tick()
	// abandons it if neither arrives within Options.ConnectTimeoutSeconds.
	const FMoQResult Result = Session->Connect();
	if (!Result.IsOk())
	{
		bConnectInFlight = false;
		ConsecutiveFailures++;
		ScheduleReconnect(Now);
		UE_LOG(LogO3DMoQSender, Warning, TEXT("moq_connect failed: %s"), *Result.Message);
		return false;
	}

	return true;
}

void FO3DMoQSender::ScheduleReconnect(double Now)
{
	NextConnectAttemptTimeSeconds = Now + ComputeBackoffDelaySeconds(ConsecutiveFailures, JitterSeed);
}

void FO3DMoQSender::HandleConnectTimeout(double Now)
{
	UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ connect to %s did not complete within %.1f s; retrying on a new client"),
		*O3DRedact::Url(Options.RelayUrl), Options.ConnectTimeoutSeconds);

	if (Session.IsValid())
	{
		Session->AbandonConnect();
	}
	bConnectInFlight = false;
	CachedState = MOQ_STATE_FAILED;
	ConsecutiveFailures++;
	DestroyPublisher();
	DestroyAudioPublisher();
	DestroyControlPublisher();
	ScheduleReconnect(Now);
	ReportSessionLost(TEXT("The MoQ connect attempt timed out."));
}

void FO3DMoQSender::HandleConnectionStateChanged(MoqConnectionState NewState)
{
	CachedState = NewState;

	switch (NewState)
	{
	case MOQ_STATE_CONNECTED:
		bConnectInFlight = false;
		ConsecutiveFailures = 0;
		UE_LOG(LogO3DMoQSender, Log, TEXT("Connected to MoQ relay %s"), *O3DRedact::Url(Options.RelayUrl));
		EnsurePublisher();
		// Audio publisher is created on-demand when CreateAudioSink is called
		if (bAudioRequested)
		{
			EnsureAudioPublisher();
		}
		// Control is announced on every connect, not on the first cue: a receiver can only
		// subscribe to an announced track, and an event sent before it subscribes is lost.
		EnsureControlPublisher();
		Queue->Wake();
		ConnectionState.Set(EO3DConnectionState::Connected);
		break;

	case MOQ_STATE_CONNECTING:
		bConnectInFlight = true;
		break;

	case MOQ_STATE_FAILED:
	case MOQ_STATE_DISCONNECTED:
		bConnectInFlight = false;
		if (NewState == MOQ_STATE_FAILED)
		{
			ConsecutiveFailures++;
		}
		// The session drops its announce cache with the connection (TRF-8), so the publishers
		// created on reconnect announce their namespaces again.
		DestroyPublisher();
		DestroyAudioPublisher();
		DestroyControlPublisher();
		ScheduleReconnect(NowSeconds());
		ReportSessionLost(NewState == MOQ_STATE_FAILED ? TEXT("The MoQ session failed.") : TEXT("The MoQ session disconnected."));
		break;

	default:
		break;
	}
}

bool FO3DMoQSender::Send(const O3DS::SubjectList& List)
{
	if (!bInitialized || !bRunning)
	{
		FO3DPerformanceMetrics::Get().RecordFrameDropped();
		return false;
	}

	FO3DPerformanceMetrics::Get().RecordFrameCaptured();
	FO3DPerformanceMetrics::Get().SetActiveSubjectCount(static_cast<int32>(List.mItems.size()));

	// Capture subject name for audio association
	FString ObservedSubject;
	if (!List.mItems.empty() && List.mItems[0])
	{
		ObservedSubject = UTF8_TO_TCHAR(List.mItems[0]->mName.c_str());
	}

	std::vector<char> Buffer;
	const double TimestampSeconds = FPlatformTime::Seconds();
	int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
	if (BytesWritten <= 0)
	{
		FO3DPerformanceMetrics::Get().RecordSerializationError();
		return false;
	}

	FO3DPerformanceMetrics::Get().RecordBytesSerialized(BytesWritten);

	TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten);
	return EnqueueFrame(MoveTemp(Bytes), MoveTemp(ObservedSubject), TimestampSeconds, /*bFullSync=*/false) == EO3DSendResult::Queued;
}

EO3DSendResult FO3DMoQSender::SendSerialized(FO3DSendPayload&& Payload)
{
	if (!bInitialized || !bRunning)
	{
		FO3DPerformanceMetrics::Get().RecordFrameDropped();
		return EO3DSendResult::NotRunning;
	}

	const int32 Len = Payload.Bytes.Num();
	if (Len <= 0)
	{
		return EO3DSendResult::Invalid;
	}

	FO3DPerformanceMetrics::Get().RecordFrameCaptured();
	FO3DPerformanceMetrics::Get().RecordBytesSerialized(Len);

	// No NotConnected: frames queue while the session (re)connects and the worker publishes or
	// drops them. The payload's bytes move into the queue without a copy.
	return EnqueueFrame(MoveTemp(Payload.Bytes), MoveTemp(Payload.Subject), Payload.CaptureTimeSec, Payload.bFullSync);
}

/**
 * Enqueues an already-serialized frame for the worker. CaptureTimestampSec is the time the caller
 * embedded in the bytes, so the enqueue-to-publish latency reflects true capture time.
 */
EO3DSendResult FO3DMoQSender::EnqueueFrame(TArray<uint8>&& Bytes, FString SubjectName, double CaptureTimestampSec, bool bFullSync)
{
	if (!SubjectName.IsEmpty())
	{
		PublishState->GetSubjectSlot().Set(SubjectName);
	}

	const int32 Len = Bytes.Num();
	const EO3DSendResult Result = Queue->Enqueue(FO3DSendItem::MakeMocap(MoveTemp(Bytes), MoveTemp(SubjectName), CaptureTimestampSec, bFullSync));
	if (Result != EO3DSendResult::Queued)
	{
		FO3DPerformanceMetrics::Get().RecordTransportFrameDropped();
		DroppedFrames.fetch_add(1);
		if (O3DMoQSenderPrivate::ClaimLogSlot(LastDropLogTimeSeconds, kDropLogIntervalSeconds))
		{
			UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ sender queue overflow (limit=%llu bytes); dropping mocap frame"), Options.MaxQueueBytes);
		}
		return Result;
	}

	FO3DPerformanceMetrics::Get().RecordBytesSent(Len);
	TransportMetrics->RecordFrameSent(static_cast<uint64>(Len));
	return EO3DSendResult::Queued;
}

/**
 * Control (ADR 0011): the envelope goes out on its own track, through the same worker queue as
 * mocap but with a cap of its own, so it never blocks. It is not a frame: no frame, byte or drop
 * counters move. Refused with NotConnected (and retried by the control publisher) until the
 * control track is announced. MoQ gives no ordering across tracks, so control is not ordered
 * against the frames around it.
 */
EO3DSendResult FO3DMoQSender::SendControl(const uint8* Envelope, int32 Len)
{
	if (!bInitialized || !bRunning)
	{
		return EO3DSendResult::NotRunning;
	}
	TConstArrayView<uint8> Payload;
	if (!O3DS::TryGetControlPayload(Envelope, Len, Payload))
	{
		return EO3DSendResult::Invalid;
	}
	if (!IsPublisherReady(ETrack::Control))
	{
		return EO3DSendResult::NotConnected;
	}
	return Queue->Enqueue(FO3DSendItem::MakeControl(TArray<uint8>(Envelope, Len)));
}

void FO3DMoQSender::Tick(float /*DeltaSeconds*/)
{
	if (!bRunning)
	{
		return;
	}

	const double Now = NowSeconds();
	if (bConnectInFlight && CachedState.Load() != MOQ_STATE_CONNECTED
		&& (Now - LastConnectAttemptTimeSeconds) >= Options.ConnectTimeoutSeconds)
	{
		HandleConnectTimeout(Now);
	}

	const MoqConnectionState State = CachedState.Load();
	if ((State == MOQ_STATE_DISCONNECTED || State == MOQ_STATE_FAILED) && !bConnectInFlight
		&& Now >= NextConnectAttemptTimeSeconds)
	{
		AttemptConnect();
	}
}

FO3DTransportStats FO3DMoQSender::GetStats() const
{
	const FO3DSendQueueStats QueueStats = Queue->GetStats();
	FO3DTransportStats Copy;
	Copy.FramesSent = FramesSent.load();
	Copy.BytesSent = BytesSent.load();
	Copy.DroppedFrames = DroppedFrames.load();
	Copy.SendErrors = SendErrors.load();
	Copy.PendingFrames = QueueStats.Mocap.PendingItems;
	Copy.PendingBytes = QueueStats.GetPendingBytes();
	{
		FScopeLock Lock(&LatencyMutex);
		if (LatencyStats.Samples > 0)
		{
			Copy.AverageLatencyMs = LatencyStats.TotalLatencyMs / static_cast<double>(LatencyStats.Samples);
			Copy.MaxLatencyMs = LatencyStats.MaxLatencyMs;
		}
	}
	Copy.State = ConnectionState.Get();
	return Copy;
}

bool FO3DMoQSender::IsPublisherReady(ETrack Track) const
{
	if (!bRunning || CachedState.Load() != MOQ_STATE_CONNECTED)
	{
		return false;
	}
	return GetPublisher(Track).IsValid();
}

TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> FO3DMoQSender::GetPublisher(ETrack Track) const
{
	FScopeLock Lock(&PublisherMutex);
	switch (Track)
	{
	case ETrack::Audio:
		return AudioPublisherHandle;
	case ETrack::Control:
		return ControlPublisherHandle;
	default:
		return MocapPublisherHandle;
	}
}

bool FO3DMoQSender::EnsurePublisher()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (GetPublisher(ETrack::Mocap).IsValid())
	{
		return true;
	}

	FMoQPublisherConfig Config;
	Config.Namespace = Options.MocapNamespace;
	Config.TrackName = Options.TrackName;
	Config.DeliveryMode = Options.DeliveryMode;

	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> NewPublisher;
	const FMoQResult Result = Session->CreatePublisher(Config, NewPublisher);
	if (!Result.IsOk())
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastErrorLogTimeSeconds) >= kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQSender, Warning, TEXT("Failed to create MoQ mocap publisher: %s"), *Result.Message);
		}
		return false;
	}

	{
		FScopeLock Lock(&PublisherMutex);
		MocapPublisherHandle = NewPublisher;
	}
	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ mocap track announced: %s/%s"), *Options.MocapNamespace, *Options.TrackName);
	return true;
}

bool FO3DMoQSender::EnsureAudioPublisher()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (GetPublisher(ETrack::Audio).IsValid())
	{
		return true;
	}

	FMoQPublisherConfig Config;
	Config.Namespace = Options.AudioNamespace;
	Config.TrackName = Options.TrackName;
	// Audio uses stream mode for reliable delivery
	Config.DeliveryMode = MOQ_DELIVERY_STREAM;

	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> NewPublisher;
	const FMoQResult Result = Session->CreatePublisher(Config, NewPublisher);
	if (!Result.IsOk())
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastErrorLogTimeSeconds) >= kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQSender, Warning, TEXT("Failed to create MoQ audio publisher: %s"), *Result.Message);
		}
		return false;
	}

	{
		FScopeLock Lock(&PublisherMutex);
		AudioPublisherHandle = NewPublisher;
	}
	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ audio track announced: %s/%s"), *Options.AudioNamespace, *Options.TrackName);
	return true;
}

bool FO3DMoQSender::EnsureControlPublisher()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (GetPublisher(ETrack::Control).IsValid())
	{
		return true;
	}

	FMoQPublisherConfig Config;
	Config.Namespace = Options.ControlNamespace;
	Config.TrackName = Options.TrackName;
	// Control uses stream mode whatever the mocap delivery mode: a cue must not be a datagram.
	Config.DeliveryMode = MOQ_DELIVERY_STREAM;

	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> NewPublisher;
	const FMoQResult Result = Session->CreatePublisher(Config, NewPublisher);
	if (!Result.IsOk())
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastControlErrorLogTimeSeconds) >= kErrorLogIntervalSeconds)
		{
			LastControlErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQSender, Warning, TEXT("Failed to create MoQ control publisher: %s"), *Result.Message);
		}
		return false;
	}

	{
		FScopeLock Lock(&PublisherMutex);
		ControlPublisherHandle = NewPublisher;
	}
	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ control track announced: %s/%s"), *Options.ControlNamespace, *Options.TrackName);
	return true;
}

void FO3DMoQSender::DestroyPublisher()
{
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> Old;
	{
		FScopeLock Lock(&PublisherMutex);
		Old = MoveTemp(MocapPublisherHandle);
		MocapPublisherHandle.Reset();
	}
	// Released outside the lock. If the worker is mid-publish it holds a snapshot, and the
	// publisher is destroyed when that snapshot goes out of scope.
	Old.Reset();
}

void FO3DMoQSender::DestroyAudioPublisher()
{
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> Old;
	{
		FScopeLock Lock(&PublisherMutex);
		Old = MoveTemp(AudioPublisherHandle);
		AudioPublisherHandle.Reset();
	}
	Old.Reset();
}

void FO3DMoQSender::DestroyControlPublisher()
{
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> Old;
	{
		FScopeLock Lock(&PublisherMutex);
		Old = MoveTemp(ControlPublisherHandle);
		ControlPublisherHandle.Reset();
	}
	Old.Reset();
}

void FO3DMoQSender::DrainQueue()
{
	// Worker joined: this thread is the queue's only consumer. Frames discarded here count as
	// dropped, as before WP-A1 PR 4e; audio and control do not (they are not frames).
	check(!Worker.IsRunning());
	DroppedFrames.fetch_add(Queue->Empty());
}

void FO3DMoQSender::SetWorkerPausedForTesting(bool bPaused)
{
	const int64 Before = PausedIterations.load();
	bWorkerPausedForTesting.store(bPaused);
	Queue->Wake();
	if (!bPaused)
	{
		return;
	}
	// An iteration that started before the store may still dequeue; the next one sees the flag.
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while (Worker.IsRunning() && PausedIterations.load() == Before && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::YieldThread();
	}
}

bool FO3DMoQSender::PublishItem(const FO3DSendItem& Item)
{
	const ETrack Track = TrackOf(Item.Kind);
	// TRF-9: publish on a snapshot taken under PublisherMutex, never on the shared member.
	const TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> Publisher = GetPublisher(Track);
	if (!Publisher.IsValid() || !Publisher->IsValid())
	{
		return false;
	}

	// Audio and control use stream mode for reliability; mocap uses the configured mode.
	const bool bControl = Track == ETrack::Control;
	const MoqDeliveryMode DeliveryMode = Track == ETrack::Mocap ? Options.DeliveryMode : MOQ_DELIVERY_STREAM;

	const FMoQResult Wrapped = Publisher->Publish(Item.Bytes.GetData(), Item.Bytes.Num(), DeliveryMode);
	if (!Wrapped.IsOk())
	{
		SendErrors.fetch_add(1);
		if (O3DMoQSenderPrivate::ClaimLogSlot(bControl ? LastControlErrorLogTimeSeconds : LastErrorLogTimeSeconds, kErrorLogIntervalSeconds))
		{
			UE_LOG(LogO3DMoQSender, Warning, TEXT("moq_publish_data failed for %s: %s"), O3DMoQSenderPrivate::TrackLabel(Item.Kind), *Wrapped.Message);
		}
		if (Track == ETrack::Mocap)
		{
			DroppedFrames.fetch_add(1);
		}
		return false;
	}

	if (Track == ETrack::Audio)
	{
		BytesSent.fetch_add(Item.Bytes.Num());
		return true;
	}
	if (bControl)
	{
		return true;
	}

	FramesSent.fetch_add(1);
	BytesSent.fetch_add(Item.Bytes.Num());
	const double LatencyMs = (FPlatformTime::Seconds() - Item.CaptureTimeSec) * 1000.0;
	FScopeLock Lock(&LatencyMutex);
	LatencyStats.TotalLatencyMs += LatencyMs;
	LatencyStats.Samples++;
	LatencyStats.MaxLatencyMs = FMath::Max(LatencyStats.MaxLatencyMs, LatencyMs);
	return true;
}

uint32 FO3DMoQSender::RunWorkerIteration()
{
	using namespace O3DMoQSenderPrivate;

	if (bWorkerPausedForTesting.load())
	{
		PausedIterations.fetch_add(1);
		return PausedWaitMs;
	}

	FO3DSendItem Item;
	if (!Queue->Dequeue(Item))
	{
		return IdleWaitMs;
	}

	if (!IsPublisherReady(TrackOf(Item.Kind)))
	{
		// Not connected, or the track not announced yet: this item is the oldest queued and is
		// dropped, so a reconnect never replays a stale backlog. Only frames are counted.
		if (Item.Kind == EO3DSendItemKind::Mocap)
		{
			DroppedFrames.fetch_add(1);
		}
		return 0;
	}

	PublishItem(Item);
	return 0;
}

void FO3DMoQSender::ResetStats()
{
	FramesSent.store(0);
	BytesSent.store(0);
	DroppedFrames.store(0);
	SendErrors.store(0);
	FScopeLock Lock(&LatencyMutex);
	LatencyStats = FLatencyStats();
}

// ─────────────────────────────────────────────────────────────────────────
// Audio Support (Phase 4)
// ─────────────────────────────────────────────────────────────────────────

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DMoQSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
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
	bAudioRequested = true;

	// Ensure audio publisher is created if connected
	if (CachedState.Load() == MOQ_STATE_CONNECTED)
	{
		EnsureAudioPublisher();
	}

	// Immutable snapshot for this sink's own encoders (TRF-10): one encoder per sink and
	// label, never reconfigured while in use.
	const FString SubjectFallback = ResolveAudioSubjectFallback();
	FO3DSinkAudioEncoder::FSettings EncoderSettings;
	EncoderSettings.Config = EffectiveConfig;
	EncoderSettings.DefaultStreamLabel = SubjectFallback;
	EncoderSettings.DefaultSubject = SubjectFallback;
	EncoderSettings.SourceGuid = AudioSourceGuid;

	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ audio sink created (codec=%s, channels=%d, rate=%d)"),
		O3DAudio::SelectCodec(EffectiveConfig) == O3DS::EUnifiedCodec::Opus ? TEXT("Opus") : TEXT("PCM16"),
		EffectiveConfig.NumChannels,
		EffectiveConfig.SampleRate);

	// The shared sink (ADR 0007 item 7): gate, per-sink encoders, bare audio payloads on the queue.
	return MakeShared<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe>(PublishState, EffectiveConfig, MoveTemp(EncoderSettings));
}

FString FO3DMoQSender::ResolveAudioSubjectFallback() const
{
	FString SubjectFallback = ActiveConfig.StreamId;
	if (SubjectFallback.IsEmpty())
	{
		SubjectFallback = Options.TrackName;
	}
	if (SubjectFallback.IsEmpty())
	{
		SubjectFallback = TEXT("moq");
	}
	return SubjectFallback;
}

#endif // O3D_WITH_TRANSPORT_MOQ
