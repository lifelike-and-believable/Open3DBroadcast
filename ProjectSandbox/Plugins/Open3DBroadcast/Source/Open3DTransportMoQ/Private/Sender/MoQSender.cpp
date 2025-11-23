#include "Sender/MoQSender.h"

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
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"

#include "o3ds/model.h"

#include <vector>

DEFINE_LOG_CATEGORY(LogO3DMoQSender);

namespace MoQSender
{
	constexpr uint64 kDefaultQueueBytes = 8ull * 1024ull * 1024ull;
	constexpr uint64 kMinQueueBytes = 256ull * 1024ull;
	constexpr uint64 kMaxQueueBytes = 256ull * 1024ull * 1024ull;
	constexpr double kErrorLogIntervalSeconds = 5.0;
	constexpr double kDropLogIntervalSeconds = 2.0;
	constexpr double kMinReconnectDelaySeconds = 0.5;
	constexpr double kMaxReconnectDelaySeconds = 10.0;

	FString GetAdvancedOption(const FO3DTransportConfig& Config, const TCHAR* Key)
	{
		for (const TPair<FString, FString>& Pair : Config.AdvancedParams)
		{
			if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				FString Value = Pair.Value;
				Value.TrimStartAndEndInline();
				return Value;
			}
		}
		return FString();
	}

	bool ParseUInt64(const FString& Input, uint64& OutValue)
	{
		if (Input.IsEmpty())
		{
			return false;
		}

		TCHAR* EndPtr = nullptr;
		OutValue = FCString::Strtoui64(*Input, &EndPtr, 10);
		return EndPtr != nullptr && *EndPtr == TEXT('\0');
	}

	FString SanitizeComponent(const FString& Value, bool bAllowSlash)
	{
		FString Result;
		Result.Reserve(Value.Len());

		for (const TCHAR Character : Value)
		{
			if (FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('-') || (bAllowSlash && Character == TEXT('/')))
			{
				Result.AppendChar(Character);
			}
			else if (FChar::IsWhitespace(Character))
			{
				Result.AppendChar(TEXT('_'));
			}
		}

		Result.TrimStartAndEndInline();
		return Result;
	}

	FString BuildDefaultNamespace(const FO3DTransportConfig& Config)
	{
		FString Namespace = GetAdvancedOption(Config, TEXT("track_namespace"));
		if (Namespace.IsEmpty())
		{
			Namespace = GetAdvancedOption(Config, TEXT("moq.namespace"));
		}

		if (Namespace.IsEmpty())
		{
			FString SessionId = GetAdvancedOption(Config, TEXT("moq.session"));
			if (SessionId.IsEmpty())
			{
				SessionId = Config.StreamId;
				SessionId.TrimStartAndEndInline();

				int32 SlashIdx = INDEX_NONE;
				if (SessionId.FindChar('/', SlashIdx))
				{
					SessionId = SessionId.Left(SlashIdx);
				}
			}

			if (SessionId.IsEmpty())
			{
				SessionId = TEXT("default");
			}

			SessionId = SanitizeComponent(SessionId, false);
			Namespace = FString::Printf(TEXT("mocap/%s"), *SessionId);
		}

		Namespace = SanitizeComponent(Namespace, true);
		if (Namespace.EndsWith(TEXT("/")))
		{
			Namespace.LeftChopInline(1);
		}

		return Namespace;
	}

	FString BuildDefaultTrackName(const FO3DTransportConfig& Config)
	{
		FString TrackName = GetAdvancedOption(Config, TEXT("track_name"));
		if (TrackName.IsEmpty())
		{
			TrackName = GetAdvancedOption(Config, TEXT("moq.track"));
		}

		if (TrackName.IsEmpty())
		{
			TrackName = Config.StreamId;
			TrackName.TrimStartAndEndInline();

			int32 SlashIdx = INDEX_NONE;
			if (TrackName.FindLastChar('/', SlashIdx))
			{
				TrackName = TrackName.Mid(SlashIdx + 1);
			}
		}

		if (TrackName.IsEmpty())
		{
			TrackName = TEXT("primary");
		}

		TrackName = SanitizeComponent(TrackName, false);
		if (TrackName.IsEmpty())
		{
			TrackName = TEXT("primary");
		}

		return TrackName;
	}
}

static FString ResolveRelayUrl(const FO3DTransportConfig& Config)
{
	FString Relay = MoQSender::GetAdvancedOption(Config, TEXT("relay_url"));
	if (Relay.IsEmpty())
	{
		Relay = MoQSender::GetAdvancedOption(Config, TEXT("moq.relay"));
	}
	if (Relay.IsEmpty())
	{
		Relay = Config.Uri;
	}

	Relay.TrimStartAndEndInline();
	return Relay;
}

static MoqDeliveryMode ResolveDeliveryMode(const FO3DTransportConfig& Config)
{
	FString Mode = MoQSender::GetAdvancedOption(Config, TEXT("delivery_mode"));
	if (Mode.IsEmpty())
	{
		Mode = MoQSender::GetAdvancedOption(Config, TEXT("moq.delivery"));
	}

	if (Mode.Equals(TEXT("datagram"), ESearchCase::IgnoreCase))
	{
		return MOQ_DELIVERY_DATAGRAM;
	}

	return MOQ_DELIVERY_STREAM;
}

static uint64 ResolveQueueBytes(const FO3DTransportConfig& Config)
{
	uint64 QueueBytes = MoQSender::kDefaultQueueBytes;
	FString QueueOverride = MoQSender::GetAdvancedOption(Config, TEXT("queue_bytes"));
	if (QueueOverride.IsEmpty())
	{
		QueueOverride = MoQSender::GetAdvancedOption(Config, TEXT("moq.queue_bytes"));
	}
	if (QueueOverride.IsEmpty())
	{
		QueueOverride = MoQSender::GetAdvancedOption(Config, TEXT("moq.qbytes"));
	}

	uint64 Parsed = 0;
	if (!QueueOverride.IsEmpty() && MoQSender::ParseUInt64(QueueOverride, Parsed))
	{
		QueueBytes = Parsed;
	}

	QueueBytes = FMath::Clamp<uint64>(QueueBytes, MoQSender::kMinQueueBytes, MoQSender::kMaxQueueBytes);
	return QueueBytes;
}

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

	Options.TrackNamespace = MoQSender::BuildDefaultNamespace(Config);
	Options.TrackName = MoQSender::BuildDefaultTrackName(Config);
	if (Options.TrackNamespace.IsEmpty() || Options.TrackName.IsEmpty())
	{
		OutError = TEXT("Unable to derive track namespace/name");
		return false;
	}

	Options.DeliveryMode = ResolveDeliveryMode(Config);
	Options.MaxQueueBytes = ResolveQueueBytes(Config);

	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ sender configured: Relay=%s Track=%s/%s Mode=%s Queue=%llu bytes"),
		*Options.RelayUrl,
		*Options.TrackNamespace,
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
	ResetStats();
	PendingQueueBytes = 0;
	DrainQueue();

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	ConsecutiveFailures = 0;
	LastConnectAttemptTimeSeconds = 0.0;
	LastErrorLogTimeSeconds = 0.0;
	LastDropLogTimeSeconds = 0.0;

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

	if (!EnqueuePayload(MoveTemp(Payload), TimestampSeconds))
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
	return PublisherHandle.IsValid();
}

bool FO3DMoQSender::EnsurePublisher()
{
	if (!Session.IsValid())
	{
		return false;
	}

	if (PublisherHandle.IsValid())
	{
		return true;
	}

	FMoQPublisherConfig Config;
	Config.Namespace = Options.TrackNamespace;
	Config.TrackName = Options.TrackName;
	Config.DeliveryMode = Options.DeliveryMode;

	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> NewPublisher;
	const FMoQResult Result = Session->CreatePublisher(Config, NewPublisher);
	if (!Result.IsOk())
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastErrorLogTimeSeconds) >= MoQSender::kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQSender, Warning, TEXT("Failed to create MoQ publisher: %s"), *Result.Message);
		}
		return false;
	}

	PublisherHandle = NewPublisher;
	UE_LOG(LogO3DMoQSender, Log, TEXT("MoQ track announced: %s/%s"), *Options.TrackNamespace, *Options.TrackName);
	return true;
}

void FO3DMoQSender::DestroyPublisher()
{
	PublisherHandle.Reset();
}

bool FO3DMoQSender::EnqueuePayload(TArray<uint8>&& Data, double CaptureTimestampSec)
{
	const uint64 PayloadBytes = Data.Num();

	{
		FScopeLock Lock(&QueueMutex);
		if ((PendingQueueBytes + PayloadBytes) > Options.MaxQueueBytes)
		{
			const double Now = FPlatformTime::Seconds();
			if ((Now - LastDropLogTimeSeconds) >= MoQSender::kDropLogIntervalSeconds)
			{
				LastDropLogTimeSeconds = Now;
				UE_LOG(LogO3DMoQSender, Warning, TEXT("MoQ sender queue overflow (limit=%llu bytes); dropping frame"), Options.MaxQueueBytes);
			}
			return false;
		}

		TUniquePtr<FPendingPayload> Payload = MakeUnique<FPendingPayload>();
		Payload->Data = MoveTemp(Data);
		Payload->EnqueueTimestampSeconds = CaptureTimestampSec;
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
	TSharedPtr<FMoQPublisherHandle, ESPMode::ThreadSafe> Publisher = PublisherHandle;
	if (!Publisher.IsValid())
	{
		return false;
	}

	MoqPublisher* RawPublisher = Publisher->Get();
	if (RawPublisher == nullptr)
	{
		return false;
	}

	const MoqResult Result = moq_publish_data(RawPublisher, Payload.Data.GetData(), Payload.Data.Num(), Options.DeliveryMode);
	if (Result.code != MOQ_OK)
	{
		const FMoQResult Wrapped = FMoQResult::FromResult(Result);
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastErrorLogTimeSeconds) >= MoQSender::kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQSender, Warning, TEXT("moq_publish_data failed: %s"), *Wrapped.Message);
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

		while (IsPublisherReady())
		{
			TUniquePtr<FPendingPayload> Payload;
			if (!DequeuePayload(Payload))
			{
				break;
			}

			if (!PublishPayload(*Payload))
			{
				break;
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

double FO3DMoQSender::ComputeReconnectDelaySeconds() const
{
	const int32 Attempts = FMath::Clamp(ConsecutiveFailures, 0, 6);
	const double Delay = 0.5 * FMath::Pow(2.0, static_cast<double>(Attempts));
	return FMath::Clamp(Delay, MoQSender::kMinReconnectDelaySeconds, MoQSender::kMaxReconnectDelaySeconds);
}
