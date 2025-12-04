#include "Sender/MoQSender.h"
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
{
	CachedState = MOQ_STATE_DISCONNECTED;
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

	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ sender configured: Relay=%s MocapTrack=%s/%s AudioTrack=%s/%s Mode=%s Queue=%llu bytes"),
		*Options.RelayUrl,
		*Options.MocapNamespace,
		*Options.TrackName,
		*Options.AudioNamespace,
		*Options.TrackName,
		Options.DeliveryMode == MOQ_DELIVERY_STREAM ? TEXT("stream") : TEXT("datagram"),
		Options.MaxQueueBytes);

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
		Session = MakeShared<FMoQSessionWrapper>();
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
	RefreshAudioEncoder();
	
	ResetStats();
	PendingQueueBytes = 0;
	DrainQueue();

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	ConsecutiveFailures = 0;
	LastConnectAttemptTimeSeconds = 0.0;
	LastErrorLogTimeSeconds = 0.0;
	LastDropLogTimeSeconds = 0.0;
	
	{
		FScopeLock Lock(&SubjectNameLock);
		LastSubjectName.Reset();
	}

	bInitialized = true;
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
	if (!bInitialized && !bRunning)
	{
		return;
	}

	bRunning = false;

	StopWorker();
	DrainQueue();
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

	bConnectInFlight = true;
	LastConnectAttemptTimeSeconds = FPlatformTime::Seconds();

	const FMoQResult Result = Session->Connect();
	if (!Result.IsOk())
	{
		bConnectInFlight = false;
		ConsecutiveFailures++;
		UE_LOG(LogO3DMoQSender, Warning, TEXT("moq_connect failed: %s"), *Result.Message);
		return false;
	}

	return true;
}

void FO3DMoQSender::HandleConnectionStateChanged(MoqConnectionState NewState)
{
	CachedState = NewState;

	switch (NewState)
	{
	case MOQ_STATE_CONNECTED:
		bConnectInFlight = false;
		ConsecutiveFailures = 0;
		UE_LOG(LogO3DMoQSender, Log, TEXT("Connected to MoQ relay %s"), *Options.RelayUrl);
		EnsurePublisher();
		// Audio publisher is created on-demand when CreateAudioSink is called
		if (bAudioEncoderInitialized)
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
		DestroyPublisher();
		DestroyAudioPublisher();
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
	if (!List.mItems.empty() && List.mItems[0])
	{
		FScopeLock Lock(&SubjectNameLock);
		LastSubjectName = UTF8_TO_TCHAR(List.mItems[0]->mName.c_str());
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

	TArray<uint8> Payload;
	Payload.SetNumUninitialized(BytesWritten);
	if (BytesWritten > 0)
	{
		FMemory::Memcpy(Payload.GetData(), Buffer.data(), BytesWritten);
	}

	if (!EnqueuePayload(MoveTemp(Payload), TimestampSeconds, /*bIsAudio=*/false))
	{
		FO3DPerformanceMetrics::Get().RecordTransportFrameDropped();
		{
			FScopeLock StatsLock(&StatsMutex);
			Stats.DroppedFrames++;
		}
		return false;
	}

	FO3DPerformanceMetrics::Get().RecordBytesSent(BytesWritten);
	FO3DPerformanceMetrics::Get().RecordTransportFrameSent(TEXT("MoQ"), BytesWritten);
	return true;
}

void FO3DMoQSender::Tick(float /*DeltaSeconds*/)
{
	if (!bRunning)
	{
		return;
	}

	const MoqConnectionState State = CachedState.Load();
	if ((State == MOQ_STATE_DISCONNECTED || State == MOQ_STATE_FAILED) && !bConnectInFlight)
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastConnectAttemptTimeSeconds) >= ComputeReconnectDelaySeconds())
		{
			AttemptConnect();
		}
	}
}

FO3DTransportStats FO3DMoQSender::GetStats() const
{
	FScopeLock Lock(&StatsMutex);
	FO3DTransportStats Copy = Stats;
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
	return MocapPublisherHandle.IsValid();
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
	return AudioPublisherHandle.IsValid();
}

bool FO3DMoQSender::EnsurePublisher()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (MocapPublisherHandle.IsValid())
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

	MocapPublisherHandle = NewPublisher;
	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ mocap track announced: %s/%s"), *Options.MocapNamespace, *Options.TrackName);
	return true;
}

bool FO3DMoQSender::EnsureAudioPublisher()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (AudioPublisherHandle.IsValid())
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

	AudioPublisherHandle = NewPublisher;
	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ audio track announced: %s/%s"), *Options.AudioNamespace, *Options.TrackName);
	return true;
}

void FO3DMoQSender::DestroyPublisher()
{
	MocapPublisherHandle.Reset();
}

void FO3DMoQSender::DestroyAudioPublisher()
{
	AudioPublisherHandle.Reset();
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
	// Select appropriate publisher based on payload type
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> Publisher = 
		Payload.bIsAudio ? AudioPublisherHandle : MocapPublisherHandle;
	
	if (!Publisher.IsValid())
	{
		return false;
	}

	MoqPublisher* RawPublisher = Publisher->Get();
	if (RawPublisher == nullptr)
	{
		return false;
	}

	// Audio uses stream mode for reliability; mocap uses configured mode
	MoqDeliveryMode DeliveryMode = Payload.bIsAudio ? MOQ_DELIVERY_STREAM : Options.DeliveryMode;
	
	const MoqResult Result = moq_publish_data(RawPublisher, Payload.Data.GetData(), Payload.Data.Num(), DeliveryMode);
	if (Result.code != MOQ_OK)
	{
		const FMoQResult Wrapped = FMoQResult::FromResult(Result);
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
		if (WakeEvent != nullptr)
		{
			WakeEvent->Wait();
		}

		if (bWorkerStopRequested)
		{
			break;
		}

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

	if (WakeEvent == nullptr)
	{
		WakeEvent = FPlatformProcess::GetSynchEventFromPool(false);
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
		if (WakeEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
			WakeEvent = nullptr;
		}
		return;
	}

	bWorkerStopRequested = true;
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
	}

	WorkerThread->WaitForCompletion();
	delete WorkerThread;
	WorkerThread = nullptr;
	WorkerRunnable.Reset();

	if (WakeEvent != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
		WakeEvent = nullptr;
	}
}

void FO3DMoQSender::WakeWorker()
{
	if (WakeEvent != nullptr)
	{
		WakeEvent->Trigger();
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
	RefreshAudioEncoder();

	// Ensure audio publisher is created if connected
	if (CachedState.Load() == MOQ_STATE_CONNECTED)
	{
		EnsureAudioPublisher();
	}

	return MakeShared<FO3DMoQSenderAudioSink, ESPMode::ThreadSafe>(*this, EffectiveConfig);
}

void FO3DMoQSender::RefreshAudioEncoder()
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

	bAudioEncoderInitialized = AudioEncoder.Initialize(ActiveAudioConfig, SubjectFallback, SubjectFallback);
	
	if (bAudioEncoderInitialized)
	{
		UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ audio encoder initialized (codec=%s, channels=%d, rate=%d)"),
			AudioEncoder.GetActiveCodec() == O3DS::EUnifiedCodec::Opus ? TEXT("Opus") : TEXT("PCM16"),
			ActiveAudioConfig.NumChannels,
			ActiveAudioConfig.SampleRate);
	}
}

bool FO3DMoQSender::ProcessCapturedAudio(const FString& StreamLabel, const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate, double TimestampSec)
{
	if (!bInitialized.Load() || !bRunning.Load() || !bAudioEncoderInitialized)
	{
		return false;
	}

	FString SubjectForAudio;
	{
		FScopeLock Lock(&SubjectNameLock);
		SubjectForAudio = LastSubjectName;
	}
	if (SubjectForAudio.IsEmpty())
	{
		SubjectForAudio = ActiveConfig.StreamId;
	}
	if (SubjectForAudio.IsEmpty())
	{
		SubjectForAudio = Options.TrackName;
	}
	if (SubjectForAudio.IsEmpty())
	{
		SubjectForAudio = TEXT("moq");
	}

	O3DAudio::FEncodedFrame Frame;
	if (!AudioEncoder.BuildEncodedFrame(StreamLabel, SubjectForAudio, Interleaved, NumFrames, NumChannels, SampleRate, TimestampSec, Frame))
	{
		return false;
	}

	if (Frame.Meta.SubjectName.IsEmpty())
	{
		Frame.Meta.SubjectName = SubjectForAudio;
	}
	Frame.Meta.SourceGuid = AudioSourceGuid;

	return SendEncodedAudio(Frame, TimestampSec);
}

bool FO3DMoQSender::SendEncodedAudio(const O3DAudio::FEncodedFrame& Frame, double TimestampSec)
{
	if (!bInitialized.Load() || !bRunning.Load() || Frame.Encoded.Num() <= 0)
	{
		return false;
	}

	// Serialize the encoded frame with metadata for transport
	// This allows the receiver to properly decode the audio data
	TArray<uint8> AudioPayload;
	if (!O3DAudio::SerializeForTransport(Frame, AudioPayload))
	{
		UE_LOG(LogO3DMoQSender, Warning, TEXT("Failed to serialize audio frame for transport"));
		return false;
	}

	if (!EnqueuePayload(MoveTemp(AudioPayload), TimestampSec, /*bIsAudio=*/true))
	{
		FScopeLock StatsLock(&StatsMutex);
		Stats.DroppedFrames++;
		return false;
	}

	return true;
}
