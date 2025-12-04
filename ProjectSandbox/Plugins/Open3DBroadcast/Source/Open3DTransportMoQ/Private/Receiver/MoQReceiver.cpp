#include "Receiver/MoQReceiver.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "SerializedFrameConsumerRegistry.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"

DEFINE_LOG_CATEGORY(LogO3DMoQReceiver);

namespace MoQReceiver
{
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
	FString Relay = MoQReceiver::GetAdvancedOption(Config, TEXT("relay_url"));
	if (Relay.IsEmpty())
	{
		Relay = MoQReceiver::GetAdvancedOption(Config, TEXT("moq.relay"));
	}
	if (Relay.IsEmpty())
	{
		Relay = Config.Uri;
	}

	Relay.TrimStartAndEndInline();
	return Relay;
}

FO3DMoQReceiver::FO3DMoQReceiver()
{
	CachedState = MOQ_STATE_DISCONNECTED;
	AliveFlag = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(true);
}

FO3DMoQReceiver::~FO3DMoQReceiver()
{
	// Mark as dead before cleanup to prevent pending callbacks from accessing this
	if (AliveFlag.IsValid())
	{
		*AliveFlag = false;
	}
	Stop();
}

bool FO3DMoQReceiver::ParseOptions(const FO3DTransportConfig& Config, FString& OutError)
{
	Options.RelayUrl = ResolveRelayUrl(Config);
	if (Options.RelayUrl.IsEmpty())
	{
		OutError = TEXT("Relay URL is required (set Uri or relay_url advanced parameter)");
		return false;
	}

	Options.TrackNamespace = MoQReceiver::BuildDefaultNamespace(Config);
	Options.TrackName = MoQReceiver::BuildDefaultTrackName(Config);
	if (Options.TrackNamespace.IsEmpty() || Options.TrackName.IsEmpty())
	{
		OutError = TEXT("Unable to derive track namespace/name");
		return false;
	}

	Options.StreamId = Config.StreamId;
	if (Options.StreamId.IsEmpty())
	{
		Options.StreamId = FString::Printf(TEXT("%s/%s"), *Options.TrackNamespace, *Options.TrackName);
	}

	UE_LOG(LogO3DMoQReceiver, Log, TEXT("MoQ receiver configured: Relay=%s Track=%s/%s StreamId=%s"),
		*Options.RelayUrl,
		*Options.TrackNamespace,
		*Options.TrackName,
		*Options.StreamId);

	return true;
}

bool FO3DMoQReceiver::Initialize(const FO3DTransportConfig& Config)
{
	if (bRunning)
	{
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver Initialize called while running"));
		return false;
	}

	FString Error;
	if (!ParseOptions(Config, Error))
	{
		UE_LOG(LogO3DMoQReceiver, Error, TEXT("MoQ receiver configuration invalid: %s"), *Error);
		return false;
	}

	if (!Session.IsValid())
	{
		Session = MakeShared<FMoQSessionWrapper>();
	}

	const FMoQResult InitResult = Session->Initialize(Options.RelayUrl);
	if (!InitResult.IsOk())
	{
		UE_LOG(LogO3DMoQReceiver, Error, TEXT("Failed to initialize MoQ session: %s"), *InitResult.Message);
		return false;
	}

	if (!ConnectionDelegateHandle.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQReceiver::HandleConnectionStateChanged);
	}

	ActiveConfig = Config;
	ResetStats();
	PendingQueueBytes = 0;

	// Drain any stale payloads
	TUniquePtr<FReceivedPayload> StalePayload;
	while (ReceiveQueue.Dequeue(StalePayload))
	{
		StalePayload.Reset();
	}

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	bSubscribed = false;
	ConsecutiveFailures = 0;
	LastConnectAttemptTimeSeconds = 0.0;
	LastSubscribeAttemptTimeSeconds = 0.0;
	LastErrorLogTimeSeconds = 0.0;

	bInitialized = true;
	return true;
}

void FO3DMoQReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

bool FO3DMoQReceiver::Start()
{
	if (!bInitialized)
	{
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver Start called before Initialize"));
		return false;
	}

	if (bRunning)
	{
		return true;
	}

	if (!ConnectionDelegateHandle.IsValid() && Session.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQReceiver::HandleConnectionStateChanged);
	}

	bRunning = true;
	if (!AttemptConnect())
	{
		UE_LOG(LogO3DMoQReceiver, Error, TEXT("Initial MoQ connection attempt failed"));
		bRunning = false;
		return false;
	}

	return true;
}

void FO3DMoQReceiver::Stop()
{
	if (!bInitialized && !bRunning)
	{
		return;
	}

	bRunning = false;

	DestroySubscriber();

	if (Session.IsValid())
	{
		if (ConnectionDelegateHandle.IsValid())
		{
			Session->OnConnectionStateChanged().Remove(ConnectionDelegateHandle);
			ConnectionDelegateHandle.Reset();
		}
		Session->Disconnect();
	}

	// Drain the receive queue
	TUniquePtr<FReceivedPayload> Payload;
	while (ReceiveQueue.Dequeue(Payload))
	{
		{
			FScopeLock Lock(&StatsMutex);
			Stats.DroppedFrames++;
		}
		Payload.Reset();
	}
	PendingQueueBytes = 0;

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	bSubscribed = false;
}

bool FO3DMoQReceiver::AttemptConnect()
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
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("moq_connect failed: %s"), *Result.Message);
		return false;
	}

	return true;
}

void FO3DMoQReceiver::HandleConnectionStateChanged(MoqConnectionState NewState)
{
	CachedState = NewState;

	switch (NewState)
	{
	case MOQ_STATE_CONNECTED:
		bConnectInFlight = false;
		ConsecutiveFailures = 0;
		UE_LOG(LogO3DMoQReceiver, Log, TEXT("Connected to MoQ relay %s"), *Options.RelayUrl);
		// Attempt to subscribe now that we're connected
		AttemptSubscribe();
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
		DestroySubscriber();
		bSubscribed = false;
		break;

	default:
		break;
	}
}

bool FO3DMoQReceiver::AttemptSubscribe()
{
	if (!Session.IsValid() || !Session->IsConnected())
	{
		return false;
	}

	if (bSubscribed && SubscriberHandle.IsValid())
	{
		return true;
	}

	LastSubscribeAttemptTimeSeconds = FPlatformTime::Seconds();

	FMoQSubscriptionConfig SubscriptionConfig;
	SubscriptionConfig.Namespace = Options.TrackNamespace;
	SubscriptionConfig.TrackName = Options.TrackName;

	// Capture AliveFlag by value to safely handle callbacks
	// This ensures that if the receiver is destroyed before the callback fires,
	// we can detect it and avoid use-after-free
	TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;
	
	SubscriptionConfig.OnData = [this, AliveFlagCopy](const TArray64<uint8>& Payload)
	{
		// Check if the receiver is still alive before accessing 'this'
		if (!AliveFlagCopy.IsValid() || !(*AliveFlagCopy))
		{
			return;
		}
		HandleDataReceived(Payload);
	};

	TSharedPtr<FMoQSubscriberHandle> NewSubscriber;
	const FMoQResult Result = Session->Subscribe(SubscriptionConfig, NewSubscriber);
	if (!Result.IsOk())
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastErrorLogTimeSeconds) >= kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQReceiver, Warning, TEXT("Failed to subscribe to %s/%s: %s"),
				*Options.TrackNamespace, *Options.TrackName, *Result.Message);
		}
		return false;
	}

	SubscriberHandle = NewSubscriber;
	bSubscribed = true;
	UE_LOG(LogO3DMoQReceiver, Log, TEXT("Subscribed to MoQ track: %s/%s"), *Options.TrackNamespace, *Options.TrackName);
	return true;
}

void FO3DMoQReceiver::HandleDataReceived(const TArray64<uint8>& Payload)
{
	if (!bRunning || Payload.IsEmpty())
	{
		return;
	}

	const uint64 PayloadBytes = static_cast<uint64>(Payload.Num());

	{
		FScopeLock Lock(&QueueMutex);
		if ((PendingQueueBytes + PayloadBytes) > kMaxQueueBytes)
		{
			// Queue overflow - drop oldest data
			UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("MoQ receiver queue overflow; dropping incoming payload"));
			{
				FScopeLock StatsLock(&StatsMutex);
				Stats.DroppedFrames++;
			}
			return;
		}

		TUniquePtr<FReceivedPayload> ReceivedPayload = MakeUnique<FReceivedPayload>();
		ReceivedPayload->Data.SetNumUninitialized(Payload.Num());
		FMemory::Memcpy(ReceivedPayload->Data.GetData(), Payload.GetData(), Payload.Num());
		ReceivedPayload->ReceiveTimestampSeconds = FPlatformTime::Seconds();

		ReceiveQueue.Enqueue(MoveTemp(ReceivedPayload));
		PendingQueueBytes += PayloadBytes;
	}
}

void FO3DMoQReceiver::DestroySubscriber()
{
	if (SubscriberHandle.IsValid())
	{
		SubscriberHandle.Reset();
	}
	bSubscribed = false;
}

int32 FO3DMoQReceiver::Poll()
{
	if (!bRunning)
	{
		return 0;
	}

	// Check for reconnection needs
	const MoqConnectionState State = CachedState.Load();
	if ((State == MOQ_STATE_DISCONNECTED || State == MOQ_STATE_FAILED) && !bConnectInFlight)
	{
		const double Now = FPlatformTime::Seconds();
		if ((Now - LastConnectAttemptTimeSeconds) >= ComputeReconnectDelaySeconds())
		{
			AttemptConnect();
		}
		return 0;
	}

	// Check if we need to resubscribe
	if (State == MOQ_STATE_CONNECTED && !bSubscribed)
	{
		AttemptSubscribe();
	}

	int32 FramesProcessed = 0;

	while (FramesProcessed < kMaxFramesPerPoll)
	{
		TUniquePtr<FReceivedPayload> Payload;
		{
			FScopeLock Lock(&QueueMutex);
			if (!ReceiveQueue.Dequeue(Payload))
			{
				break;
			}
			PendingQueueBytes = (PendingQueueBytes >= static_cast<uint64>(Payload->Data.Num()))
				? (PendingQueueBytes - Payload->Data.Num())
				: 0;
		}

		if (ProcessReceivedPayload(*Payload))
		{
			FScopeLock Lock(&StatsMutex);
			Stats.FramesReceived++;
			Stats.BytesReceived += Payload->Data.Num();
			++FramesProcessed;

			// Track latency
			const double LatencyMs = (FPlatformTime::Seconds() - Payload->ReceiveTimestampSeconds) * 1000.0;
			LatencyStats.TotalLatencyMs += LatencyMs;
			LatencyStats.Samples++;
			LatencyStats.MaxLatencyMs = FMath::Max(LatencyStats.MaxLatencyMs, LatencyMs);
		}
		else
		{
			FScopeLock Lock(&StatsMutex);
			Stats.DroppedFrames++;
		}
	}

	return FramesProcessed;
}

bool FO3DMoQReceiver::ProcessReceivedPayload(const FReceivedPayload& Payload)
{
	if (Payload.Data.IsEmpty())
	{
		return false;
	}

	TSharedPtr<ISerializedFrameConsumer> ConsumerPinned = Consumer.Pin();
	if (!ConsumerPinned.IsValid())
	{
		return false;
	}

	// Submit the frame to the consumer
	TArray<uint8> FrameData;
	FrameData.SetNumUninitialized(Payload.Data.Num());
	FMemory::Memcpy(FrameData.GetData(), Payload.Data.GetData(), Payload.Data.Num());
	
	ConsumerPinned->SubmitFrame(Options.StreamId, FrameData, Payload.ReceiveTimestampSeconds);
	return true;
}

FO3DTransportStats FO3DMoQReceiver::GetStats() const
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

void FO3DMoQReceiver::ResetStats()
{
	FScopeLock Lock(&StatsMutex);
	Stats.Reset();
	LatencyStats = FLatencyStats();
}

double FO3DMoQReceiver::ComputeReconnectDelaySeconds() const
{
	const int32 Attempts = FMath::Clamp(ConsecutiveFailures, 0, 6);
	const double Delay = 0.5 * FMath::Pow(2.0, static_cast<double>(Attempts));
	return FMath::Clamp(Delay, kMinReconnectDelaySeconds, kMaxReconnectDelaySeconds);
}
