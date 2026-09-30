#include "Sender/MoQSender.h"
#include "O3DRedact.h"
#include "Sender/MoQSenderAudioSink.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeLock.h"
#include "O3DPerformanceMetrics.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQHelpers.h"
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"

#include "o3ds/model.h"

#include <vector>

DEFINE_LOG_CATEGORY(LogO3DMoQSender);

// Use constants from MoQHelpers
using namespace MoQHelpers;

class FSendWorker : public FRunnable
{
public:
	explicit FSendWorker(FO3DMoQSender& InOwner)
		: Owner(InOwner)
	{
	}

	virtual uint32 Run() override
	{
		return Owner.RunWorker();
	}

	virtual void Stop() override
	{
		// Owner coordinates stop via bWorkerStopRequested flag.
	}

private:
	FO3DMoQSender& Owner;
};

FO3DMoQSender::FO3DMoQSender()
	// The cycle counter differs per instance and per run, so many senders do not retry in lockstep.
	: FO3DMoQSender(FMoQFfiApi::GetProduction(), nullptr, FPlatformTime::Cycles64())
{
}

FO3DMoQSender::FO3DMoQSender(FMoQFfiApiRef InApi, TFunction<double()> InClock, uint64 InJitterSeed)
	: Api(MoveTemp(InApi))
	, Clock(MoveTemp(InClock))
	, JitterSeed(InJitterSeed)
	, AudioState(MakeShared<FMoQSenderAudioState, ESPMode::ThreadSafe>())
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
	Options.TrackName = BuildDefaultTrackName(Config);
	
	if (Options.MocapNamespace.IsEmpty() || Options.TrackName.IsEmpty())
	{
		OutError = TEXT("Unable to derive track namespace/name");
		return false;
	}

	Options.DeliveryMode = ResolveDeliveryMode(Config);
	Options.MaxQueueBytes = ResolveQueueBytes(Config);
	Options.ConnectTimeoutSeconds = ResolveConnectTimeoutSeconds(Config);

	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ sender configured: Relay=%s MocapTrack=%s/%s AudioTrack=%s/%s Mode=%s Queue=%llu bytes ConnectTimeout=%.1fs"),
		*O3DRedact::Url(Options.RelayUrl),
		*Options.MocapNamespace,
		*Options.TrackName,
		*Options.AudioNamespace,
		*Options.TrackName,
		Options.DeliveryMode == MOQ_DELIVERY_STREAM ? TEXT("stream") : TEXT("datagram"),
		Options.MaxQueueBytes,
		Options.ConnectTimeoutSeconds);

	return true;
}

bool FO3DMoQSender::Initialize(const FO3DTransportConfig& Config)
{
	if (bRunning)
	{
		UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ sender Initialize called while running"));
		return false;
	}

	FString Error;
	if (!ParseOptions(Config, Error))
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("MoQ sender configuration invalid: %s"), *Error);
		return false;
	}

	if (!Session.IsValid())
	{
		Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>(Api);
	}

	const FMoQResult InitResult = Session->Initialize(Options.RelayUrl);
	if (!InitResult.IsOk())
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("Failed to initialize MoQ session: %s"), *InitResult.Message);
		return false;
	}

	if (!ConnectionDelegateHandle.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQSender::HandleConnectionStateChanged);
	}

	ActiveConfig = Config;
	ActiveAudioConfig = Config.Audio;
	AudioSourceGuid = FGuid::NewGuid();
	bAudioRequested = false;

	ResetStats();
	PendingQueueBytes = 0;
	DrainQueue();
	AudioState->AudioQueue.Empty();
	AudioState->AudioDropped.store(0);

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	ConsecutiveFailures = 0;
	LastConnectAttemptTimeSeconds = 0.0;
	NextConnectAttemptTimeSeconds = 0.0;
	LastErrorLogTimeSeconds = 0.0;
	LastDropLogTimeSeconds = 0.0;
	
	AudioState->LastSubject.Reset();

	bInitialized = true;
	AudioState->Gate->Open();
	return true;
}

bool FO3DMoQSender::Start()
{
	if (!bInitialized)
	{
		UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ sender Start called before Initialize"));
		return false;
	}

	if (bRunning)
	{
		return true;
	}

	if (!ConnectionDelegateHandle.IsValid() && Session.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQSender::HandleConnectionStateChanged);
	}

	AudioState->Gate->Open();

	StartWorker();
	if (WorkerThread == nullptr)
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("Failed to start MoQ sender worker thread"));
		return false;
	}

	bRunning = true;
	if (!AttemptConnect())
	{
		UE_LOG(LogO3DMoQSender, Error, TEXT("Initial MoQ connection attempt failed"));
		bRunning = false;
		StopWorker();
		return false;
	}

	return true;
}

void FO3DMoQSender::Stop()
{
	// WP-S5 ordering: close the audio gate first (waits for in-flight submits), then stop the
	// worker, then release publishers and the session.
	AudioState->Gate->Close();

	if (!bInitialized && !bRunning)
	{
		return;
	}

	bRunning = false;

	StopWorker();
	DrainQueue();
	DrainAudioQueue(/*bPublish=*/false);
	DestroyPublisher();
	DestroyAudioPublisher();

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
	ScheduleReconnect(Now);
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
		WakeWorker();
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
		ScheduleReconnect(NowSeconds());
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

	return SendBytes(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten, ObservedSubject, TimestampSeconds);
}

bool FO3DMoQSender::SendSerialized(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec)
{
	if (!bInitialized || !bRunning)
	{
		FO3DPerformanceMetrics::Get().RecordFrameDropped();
		return false;
	}

	if (Len <= 0)
	{
		return false;
	}

	FO3DPerformanceMetrics::Get().RecordFrameCaptured();
	FO3DPerformanceMetrics::Get().RecordBytesSerialized(Len);

	return SendBytes(Data, Len, SubjectName, CaptureTimestampSec);
}

/** Enqueue an already-serialized payload for the send worker and record transport-level stats/subject bookkeeping.
 *  CaptureTimestampSec is the same value the caller already embedded in Data (Send()'s own
 *  FPlatformTime::Seconds() call, or FO3DSenderSerializer's `Now` via SendSerialized()) - reused for
 *  EnqueuePayload()'s capture timestamp so the enqueue-to-publish latency measurement below reflects
 *  true frame-capture time rather than "whenever SendBytes() happened to run". */
bool FO3DMoQSender::SendBytes(const uint8* Data, int32 Len, const FString& SubjectName, double CaptureTimestampSec)
{
	if (!SubjectName.IsEmpty())
	{
		AudioState->LastSubject.Set(SubjectName);
	}

	TArray<uint8> Payload;
	Payload.SetNumUninitialized(Len);
	FMemory::Memcpy(Payload.GetData(), Data, Len);

	if (!EnqueuePayload(MoveTemp(Payload), CaptureTimestampSec, /*bIsAudio=*/false))
	{
		FO3DPerformanceMetrics::Get().RecordTransportFrameDropped();
		{
			FScopeLock StatsLock(&StatsMutex);
			Stats.DroppedFrames++;
		}
		return false;
	}

	FO3DPerformanceMetrics::Get().RecordBytesSent(Len);
	TransportMetrics->RecordFrameSent(static_cast<uint64>(Len));
	return true;
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
	FScopeLock Lock(&StatsMutex);
	FO3DTransportStats Copy = Stats;
	Copy.DroppedFrames += AudioState->AudioDropped.load();
	if (LatencyStats.Samples > 0)
	{
		Copy.AverageLatencyMs = LatencyStats.TotalLatencyMs / static_cast<double>(LatencyStats.Samples);
		Copy.MaxLatencyMs = LatencyStats.MaxLatencyMs;
	}
	return Copy;
}

bool FO3DMoQSender::IsPublisherReady() const
{
	if (!bRunning)
	{
		return false;
	}
	if (CachedState.Load() != MOQ_STATE_CONNECTED)
	{
		return false;
	}
	return GetPublisher(/*bAudio=*/false).IsValid();
}

bool FO3DMoQSender::IsAudioPublisherReady() const
{
	if (!bRunning)
	{
		return false;
	}
	if (CachedState.Load() != MOQ_STATE_CONNECTED)
	{
		return false;
	}
	return GetPublisher(/*bAudio=*/true).IsValid();
}

TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> FO3DMoQSender::GetPublisher(bool bAudio) const
{
	FScopeLock Lock(&PublisherMutex);
	return bAudio ? AudioPublisherHandle : MocapPublisherHandle;
}

bool FO3DMoQSender::EnsurePublisher()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (GetPublisher(/*bAudio=*/false).IsValid())
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

	if (GetPublisher(/*bAudio=*/true).IsValid())
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

bool FO3DMoQSender::EnqueuePayload(TArray<uint8>&& Data, double CaptureTimestampSec, bool bIsAudio)
{
	const uint64 PayloadBytes = Data.Num();

	{
		FScopeLock Lock(&QueueMutex);
		if ((PendingQueueBytes + PayloadBytes) > Options.MaxQueueBytes)
		{
			const double Now = FPlatformTime::Seconds();
			if ((Now - LastDropLogTimeSeconds) >= kDropLogIntervalSeconds)
			{
				LastDropLogTimeSeconds = Now;
				UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ sender queue overflow (limit=%llu bytes); dropping %s frame"), 
					Options.MaxQueueBytes, bIsAudio ? TEXT("audio") : TEXT("mocap"));
			}
			return false;
		}

		TUniquePtr<FPendingPayload> Payload = MakeUnique<FPendingPayload>();
		Payload->Data = MoveTemp(Data);
		Payload->EnqueueTimestampSeconds = CaptureTimestampSec;
		Payload->bIsAudio = bIsAudio;
		SendQueue.Enqueue(MoveTemp(Payload));
		PendingQueueBytes += PayloadBytes;
	}

	WakeWorker();
	return true;
}

bool FO3DMoQSender::DequeuePayload(TUniquePtr<FPendingPayload>& OutPayload)
{
	FScopeLock Lock(&QueueMutex);
	if (!SendQueue.Dequeue(OutPayload))
	{
		return false;
	}

	PendingQueueBytes = (PendingQueueBytes >= static_cast<uint64>(OutPayload->Data.Num()))
		? (PendingQueueBytes - OutPayload->Data.Num())
		: 0;
	return true;
}

void FO3DMoQSender::DrainQueue()
{
	TUniquePtr<FPendingPayload> Payload;
	while (DequeuePayload(Payload))
	{
		FScopeLock StatsLock(&StatsMutex);
		Stats.DroppedFrames++;
		Payload.Reset();
	}
}

bool FO3DMoQSender::PublishPayload(const FPendingPayload& Payload)
{
	// TRF-9: publish on a snapshot taken under PublisherMutex, never on the shared member.
	const TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> Publisher = GetPublisher(Payload.bIsAudio);
	if (!Publisher.IsValid() || !Publisher->IsValid())
	{
		return false;
	}

	// Audio uses stream mode for reliability; mocap uses configured mode
	MoqDeliveryMode DeliveryMode = Payload.bIsAudio ? MOQ_DELIVERY_STREAM : Options.DeliveryMode;

	const FMoQResult Wrapped = Publisher->Publish(Payload.Data.GetData(), Payload.Data.Num(), DeliveryMode);
	if (!Wrapped.IsOk())
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastErrorLogTimeSeconds) >= kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQSender, Warning, TEXT("moq_publish_data failed for %s: %s"), 
				Payload.bIsAudio ? TEXT("audio") : TEXT("mocap"), *Wrapped.Message);
		}

		FScopeLock StatsLock(&StatsMutex);
		Stats.DroppedFrames++;
		return false;
	}

	const double LatencyMs = (FPlatformTime::Seconds() - Payload.EnqueueTimestampSeconds) * 1000.0;

	FScopeLock StatsLock(&StatsMutex);
	Stats.FramesSent++;
	Stats.BytesSent += Payload.Data.Num();
	LatencyStats.TotalLatencyMs += LatencyMs;
	LatencyStats.Samples++;
	LatencyStats.MaxLatencyMs = FMath::Max(LatencyStats.MaxLatencyMs, LatencyMs);

	return true;
}

uint32 FO3DMoQSender::RunWorker()
{
	while (!bWorkerStopRequested)
	{
		// Shared wake event: mocap enqueues and audio sinks both trigger it.
		AudioState->AudioQueue.WaitForWork(100);

		if (bWorkerStopRequested)
		{
			break;
		}

		DrainAudioQueue(/*bPublish=*/true);

		// Process all queued payloads - each may go to mocap or audio track
		while (true)
		{
			TUniquePtr<FPendingPayload> Payload;
			if (!DequeuePayload(Payload))
			{
				break;
			}

			// Check if appropriate publisher is ready
			bool bPublisherReady = Payload->bIsAudio ? IsAudioPublisherReady() : IsPublisherReady();
			if (!bPublisherReady)
			{
				// Drop the payload if publisher not ready
				FScopeLock StatsLock(&StatsMutex);
				Stats.DroppedFrames++;
				continue;
			}

			if (!PublishPayload(*Payload))
			{
				// PublishPayload already logs and updates stats on failure
				continue;
			}
		}
	}

	return 0;
}

void FO3DMoQSender::StartWorker()
{
	if (WorkerThread != nullptr)
	{
		return;
	}

	bWorkerStopRequested = false;
	WorkerRunnable = MakeUnique<FSendWorker>(*this);
	WorkerThread = FRunnableThread::Create(WorkerRunnable.Get(), TEXT("MoQSenderWorker"), 0, TPri_AboveNormal);
	if (WorkerThread == nullptr)
	{
		WorkerRunnable.Reset();
		bWorkerStopRequested = true;
	}
}

void FO3DMoQSender::StopWorker()
{
	if (WorkerThread == nullptr)
	{
		return;
	}

	bWorkerStopRequested = true;
	AudioState->AudioQueue.Wake();

	WorkerThread->WaitForCompletion();
	delete WorkerThread;
	WorkerThread = nullptr;
	WorkerRunnable.Reset();
}

void FO3DMoQSender::WakeWorker()
{
	AudioState->AudioQueue.Wake();
}

void FO3DMoQSender::DrainAudioQueue(bool bPublish)
{
	TArray<uint8> Bytes;
	while (AudioState->AudioQueue.Dequeue(Bytes))
	{
		if (!bPublish || !IsAudioPublisherReady())
		{
			FScopeLock StatsLock(&StatsMutex);
			Stats.DroppedFrames++;
			continue;
		}

		FPendingPayload Payload;
		Payload.Data = MoveTemp(Bytes);
		Payload.EnqueueTimestampSeconds = FPlatformTime::Seconds();
		Payload.bIsAudio = true;
		PublishPayload(Payload);
	}
}

void FO3DMoQSender::ResetStats()
{
	FScopeLock Lock(&StatsMutex);
	Stats.Reset();
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

	return MakeShared<FO3DMoQSenderAudioSink, ESPMode::ThreadSafe>(AudioState, EffectiveConfig, MoveTemp(EncoderSettings));
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
