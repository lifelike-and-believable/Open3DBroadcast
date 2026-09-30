// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Receiver/MoQReceiver.h"
#include "O3DRedact.h"

#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeLock.h"
#include "Transport/O3DSerializedFrameConsumer.h"
#include "Shared/MoQHandles.h"
#include "Shared/MoQHelpers.h"
#include "Shared/MoQSessionWrapper.h"
#include "Shared/MoQTypes.h"
#include "O3DAudioFrameCodec.h"
#include "O3DAudioSerialization.h"
#include "O3DUnifiedMessage.h"

DEFINE_LOG_CATEGORY(LogO3DMoQReceiver);

// Use constants from MoQHelpers
using namespace MoQHelpers;

FO3DMoQReceiver::FO3DMoQReceiver()
	// The cycle counter differs per instance and per run, so many receivers do not retry in lockstep.
	: FO3DMoQReceiver(FMoQFfiApi::GetProduction(), nullptr, FPlatformTime::Cycles64())
{
}

FO3DMoQReceiver::FO3DMoQReceiver(FMoQFfiApiRef InApi, TFunction<double()> InClock, uint64 InJitterSeed)
	: Api(MoveTemp(InApi))
	, Clock(MoveTemp(InClock))
	, JitterSeed(InJitterSeed)
{
	CachedState = MOQ_STATE_DISCONNECTED;
	AliveFlag = MakeShared<FThreadSafeBool, ESPMode::ThreadSafe>(true);
}

double FO3DMoQReceiver::NowSeconds() const
{
	return Clock ? Clock() : FPlatformTime::Seconds();
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

	// Build separate namespaces for mocap and audio tracks
	Options.MocapNamespace = BuildDefaultMocapNamespace(Config);
	Options.AudioNamespace = BuildDefaultAudioNamespace(Config);
	Options.TrackName = BuildDefaultTrackName(Config);
	
	if (Options.MocapNamespace.IsEmpty() || Options.TrackName.IsEmpty())
	{
		OutError = TEXT("Unable to derive track namespace/name");
		return false;
	}

	Options.ConnectTimeoutSeconds = ResolveConnectTimeoutSeconds(Config);

	Options.StreamId = Config.StreamId;
	if (Options.StreamId.IsEmpty())
	{
		Options.StreamId = FString::Printf(TEXT("%s/%s"), *Options.MocapNamespace, *Options.TrackName);
	}

	UE_LOG(LogO3DMoQReceiver, Log, TEXT("MoQ receiver configured: Relay=%s MocapTrack=%s/%s AudioTrack=%s/%s StreamId=%s"),
		*O3DRedact::Url(Options.RelayUrl),
		*Options.MocapNamespace,
		*Options.TrackName,
		*Options.AudioNamespace,
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
		Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>(Api);
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
	ActiveAudioConfig = Config.Audio;
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
	bMocapSubscribed = false;
	bAudioSubscribed = false;
	ConsecutiveFailures = 0;
	LastConnectAttemptTimeSeconds = 0.0;
	NextConnectAttemptTimeSeconds = 0.0;
	LastSubscribeAttemptTimeSeconds = 0.0;
	MocapSubscribeRetry.Reset();
	AudioSubscribeRetry.Reset();
	LastErrorLogTimeSeconds = 0.0;

	bInitialized = true;
	return true;
}

void FO3DMoQReceiver::SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& InConsumer)
{
	Consumer = InConsumer;
}

void FO3DMoQReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
	AudioSink = Sink;
	if (Sink.IsValid())
	{
		ActiveAudioConfig = AudioConfig;
		// If already connected, try to subscribe to audio track
		if (CachedState.Load() == MOQ_STATE_CONNECTED && !bAudioSubscribed)
		{
			AttemptAudioSubscribe();
		}
	}
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
	DestroyAudioSubscriber();

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
	bMocapSubscribed = false;
	bAudioSubscribed = false;
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

	const double Now = NowSeconds();
	bConnectInFlight = true;
	LastConnectAttemptTimeSeconds = Now;

	// Each attempt runs on a fresh client and ends in CONNECTED or FAILED (TRF-11); Poll()
	// abandons it if neither arrives within Options.ConnectTimeoutSeconds.
	const FMoQResult Result = Session->Connect();
	if (!Result.IsOk())
	{
		bConnectInFlight = false;
		ConsecutiveFailures++;
		ScheduleReconnect(Now);
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("moq_connect failed: %s"), *Result.Message);
		return false;
	}

	return true;
}

void FO3DMoQReceiver::ScheduleReconnect(double Now)
{
	NextConnectAttemptTimeSeconds = Now + ComputeBackoffDelaySeconds(ConsecutiveFailures, JitterSeed);
}

void FO3DMoQReceiver::ScheduleSubscribeRetry(FSubscribeRetryState& Retry, double Now, uint64 SeedSalt)
{
	++Retry.ConsecutiveFailures;
	Retry.NextAttemptTimeSeconds = Now + ComputeBackoffDelaySeconds(Retry.ConsecutiveFailures, JitterSeed ^ SeedSalt);
}

void FO3DMoQReceiver::HandleConnectTimeout(double Now)
{
	UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ connect to %s did not complete within %.1f s; retrying on a new client"),
		*O3DRedact::Url(Options.RelayUrl), Options.ConnectTimeoutSeconds);

	if (Session.IsValid())
	{
		Session->AbandonConnect();
	}
	bConnectInFlight = false;
	CachedState = MOQ_STATE_FAILED;
	ConsecutiveFailures++;
	DestroySubscriber();
	DestroyAudioSubscriber();
	ScheduleReconnect(Now);
}

void FO3DMoQReceiver::HandleConnectionStateChanged(MoqConnectionState NewState)
{
	CachedState = NewState;

	switch (NewState)
	{
	case MOQ_STATE_CONNECTED:
		bConnectInFlight = false;
		ConsecutiveFailures = 0;
		UE_LOG(LogO3DMoQReceiver, Log, TEXT("Connected to MoQ relay %s"), *O3DRedact::Url(Options.RelayUrl));
		// A new connection gets an immediate subscribe; retries after that back off (TRF-20).
		MocapSubscribeRetry.Reset();
		AudioSubscribeRetry.Reset();
		// Subscribe to mocap track
		AttemptSubscribe();
		// Subscribe to audio track if audio sink is configured
		if (AudioSink.IsValid())
		{
			AttemptAudioSubscribe();
		}
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
		DestroyAudioSubscriber();
		bMocapSubscribed = false;
		bAudioSubscribed = false;
		ScheduleReconnect(NowSeconds());
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

	if (bMocapSubscribed && MocapSubscriberHandle.IsValid())
	{
		return true;
	}

	LastSubscribeAttemptTimeSeconds = NowSeconds();

	FMoQSubscriptionConfig SubscriptionConfig;
	SubscriptionConfig.Namespace = Options.MocapNamespace;
	SubscriptionConfig.TrackName = Options.TrackName;

	// Capture AliveFlag by value to safely handle callbacks
	TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;
	
	SubscriptionConfig.OnData = [this, AliveFlagCopy](const TArray64<uint8>& Payload)
	{
		if (!AliveFlagCopy.IsValid() || !(*AliveFlagCopy))
		{
			return;
		}
		HandleMocapDataReceived(Payload);
	};

	TSharedPtr<FMoQSubscriberHandle> NewSubscriber;
	const FMoQResult Result = Session->Subscribe(SubscriptionConfig, NewSubscriber);
	if (!Result.IsOk())
	{
		const double Now = NowSeconds();
		ScheduleSubscribeRetry(MocapSubscribeRetry, Now, /*SeedSalt=*/0x6D6F636170ull);
		if ((Now - LastErrorLogTimeSeconds) >= kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQReceiver, Warning, TEXT("Failed to subscribe to mocap track %s/%s (retry in %.2f s): %s"),
				*Options.MocapNamespace, *Options.TrackName, MocapSubscribeRetry.NextAttemptTimeSeconds - Now, *Result.Message);
		}
		return false;
	}

	MocapSubscribeRetry.Reset();
	MocapSubscriberHandle = NewSubscriber;
	bMocapSubscribed = true;
	UE_LOG(LogO3DMoQReceiver, Log, TEXT("Subscribed to MoQ mocap track: %s/%s"), *Options.MocapNamespace, *Options.TrackName);
	return true;
}

bool FO3DMoQReceiver::AttemptAudioSubscribe()
{
	if (!Session.IsValid() || !Session->IsConnected())
	{
		return false;
	}

	if (bAudioSubscribed && AudioSubscriberHandle.IsValid())
	{
		return true;
	}

	FMoQSubscriptionConfig SubscriptionConfig;
	SubscriptionConfig.Namespace = Options.AudioNamespace;
	SubscriptionConfig.TrackName = Options.TrackName;

	// Capture AliveFlag by value to safely handle callbacks
	TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;
	
	SubscriptionConfig.OnData = [this, AliveFlagCopy](const TArray64<uint8>& Payload)
	{
		if (!AliveFlagCopy.IsValid() || !(*AliveFlagCopy))
		{
			return;
		}
		HandleAudioDataReceived(Payload);
	};

	TSharedPtr<FMoQSubscriberHandle> NewSubscriber;
	const FMoQResult Result = Session->Subscribe(SubscriptionConfig, NewSubscriber);
	if (!Result.IsOk())
	{
		const double Now = NowSeconds();
		ScheduleSubscribeRetry(AudioSubscribeRetry, Now, /*SeedSalt=*/0x617564696Full);
		if ((Now - LastErrorLogTimeSeconds) >= kErrorLogIntervalSeconds)
		{
			LastErrorLogTimeSeconds = Now;
			UE_LOG(LogO3DMoQReceiver, Warning, TEXT("Failed to subscribe to audio track %s/%s (retry in %.2f s): %s"),
				*Options.AudioNamespace, *Options.TrackName, AudioSubscribeRetry.NextAttemptTimeSeconds - Now, *Result.Message);
		}
		return false;
	}

	AudioSubscribeRetry.Reset();
	AudioSubscriberHandle = NewSubscriber;
	bAudioSubscribed = true;
	UE_LOG(LogO3DMoQReceiver, Log, TEXT("Subscribed to MoQ audio track: %s/%s"), *Options.AudioNamespace, *Options.TrackName);
	return true;
}

void FO3DMoQReceiver::HandleMocapDataReceived(const TArray64<uint8>& Payload)
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
			UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("MoQ receiver queue overflow; dropping incoming mocap payload"));
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
		ReceivedPayload->bIsAudio = false;

		ReceiveQueue.Enqueue(MoveTemp(ReceivedPayload));
		PendingQueueBytes += PayloadBytes;
	}
}

void FO3DMoQReceiver::HandleAudioDataReceived(const TArray64<uint8>& Payload)
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
			UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("MoQ receiver queue overflow; dropping incoming audio payload"));
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
		ReceivedPayload->bIsAudio = true;

		ReceiveQueue.Enqueue(MoveTemp(ReceivedPayload));
		PendingQueueBytes += PayloadBytes;
	}
}

void FO3DMoQReceiver::DestroySubscriber()
{
	if (MocapSubscriberHandle.IsValid())
	{
		MocapSubscriberHandle.Reset();
	}
	bMocapSubscribed = false;
}

void FO3DMoQReceiver::DestroyAudioSubscriber()
{
	if (AudioSubscriberHandle.IsValid())
	{
		AudioSubscriberHandle.Reset();
	}
	bAudioSubscribed = false;
}

int32 FO3DMoQReceiver::Poll()
{
	if (!bRunning)
	{
		return 0;
	}

	const double Now = NowSeconds();
	if (bConnectInFlight && CachedState.Load() != MOQ_STATE_CONNECTED
		&& (Now - LastConnectAttemptTimeSeconds) >= Options.ConnectTimeoutSeconds)
	{
		HandleConnectTimeout(Now);
	}

	// Check for reconnection needs
	const MoqConnectionState State = CachedState.Load();
	if ((State == MOQ_STATE_DISCONNECTED || State == MOQ_STATE_FAILED) && !bConnectInFlight)
	{
		if (Now >= NextConnectAttemptTimeSeconds)
		{
			AttemptConnect();
		}
		return 0;
	}

	// Resubscribe with backoff (TRF-20): moq_subscribe is synchronous, so a track that is not
	// announced yet must not be retried every Poll().
	if (State == MOQ_STATE_CONNECTED && !bMocapSubscribed && Now >= MocapSubscribeRetry.NextAttemptTimeSeconds)
	{
		AttemptSubscribe();
	}
	if (State == MOQ_STATE_CONNECTED && !bAudioSubscribed && AudioSink.IsValid() && Now >= AudioSubscribeRetry.NextAttemptTimeSeconds)
	{
		AttemptAudioSubscribe();
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

		bool bProcessed = false;
		if (Payload->bIsAudio)
		{
			bProcessed = ProcessAudioPayload(*Payload);
		}
		else
		{
			bProcessed = ProcessReceivedPayload(*Payload);
		}

		if (bProcessed)
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

bool FO3DMoQReceiver::ProcessAudioPayload(const FReceivedPayload& Payload)
{
	if (Payload.Data.IsEmpty())
	{
		return false;
	}

	TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe> SinkPinned = AudioSink.Pin();
	if (!SinkPinned.IsValid())
	{
		// No audio sink configured - silently drop
		return true;
	}

	// TRF-37: decode with the codec the sender wrote into the frame header. The local config
	// may differ from the sender's, which used to make every frame fail to deserialize.
	O3DS::EUnifiedCodec Codec = O3DS::EUnifiedCodec::PCM16;
	if (!TryGetAudioCodecFromFrame(Payload.Data.GetData(), Payload.Data.Num(), Codec))
	{
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver got an audio frame with an unknown header (payload=%d)."), Payload.Data.Num());
		return false;
	}

	// Deserialize the encoded audio frame (produced by O3DAudio::SerializeForTransport on sender)
	O3DAudio::FEncodedAudioFrame EncodedFrame;
	if (!O3DAudio::DeserializeEncodedAudioFrame(Codec, Payload.Data.GetData(), Payload.Data.Num(), EncodedFrame))
	{
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver failed to deserialize audio frame (payload=%d codec=%d)."), 
			Payload.Data.Num(), static_cast<int32>(Codec));
		return false;
	}

	// PCM16 can be submitted directly
	if (Codec == O3DS::EUnifiedCodec::PCM16)
	{
		SinkPinned->SubmitPcm16(EncodedFrame.Meta, EncodedFrame.Payload.GetData(), EncodedFrame.Payload.Num());
		return true;
	}

	// Decode Opus (or other codecs) to PCM16
	if (!AudioDecoder.Decode(Codec, EncodedFrame.Meta, EncodedFrame.Payload.GetData(), EncodedFrame.Payload.Num(), DecodedPcmScratch))
	{
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver failed to decode audio frame (codec=%d)."), static_cast<int32>(Codec));
		return false;
	}

	SinkPinned->SubmitPcm16(EncodedFrame.Meta,
		reinterpret_cast<const uint8*>(DecodedPcmScratch.GetData()),
		DecodedPcmScratch.Num() * sizeof(int16));
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

#endif // O3D_WITH_TRANSPORT_MOQ
