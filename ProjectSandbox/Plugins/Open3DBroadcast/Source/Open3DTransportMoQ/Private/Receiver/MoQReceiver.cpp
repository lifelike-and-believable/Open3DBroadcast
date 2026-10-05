// Copyright 2026 Lifelike & Believable. All Rights Reserved.

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

	// Hand-off limits, as before WP-A1 PR 4e (16 MiB), per kind now: a burst on one track cannot
	// refuse another. Nothing queued is ever discarded.
	FO3DSendQueueLimits Limits;
	Limits.Mocap.MaxBytes = kMaxQueueBytes;
	Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
	Limits.Audio.MaxBytes = kMaxQueueBytes;
	ReceiveQueue.SetLimits(Limits);
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
	Options.ControlNamespace = BuildDefaultControlNamespace(Config);
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

FO3DTransportResult FO3DMoQReceiver::Initialize(const FO3DTransportConfig& Config)
{
	if (bRunning)
	{
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver Initialize called while running"));
		return FO3DTransportResult::Error(EO3DTransportError::Internal, TEXT("MoQ receiver Initialize() while running; Stop() it first."));
	}

	FString Error;
	if (!ParseOptions(Config, Error))
	{
		UE_LOG(LogO3DMoQReceiver, Error, TEXT("MoQ receiver configuration invalid: %s"), *Error);
		return FO3DTransportResult::Error(EO3DTransportError::InvalidConfig, FString::Printf(TEXT("MoQ receiver configuration invalid: %s"), *Error));
	}

	if (!Session.IsValid())
	{
		Session = MakeShared<FMoQSessionWrapper, ESPMode::ThreadSafe>(Api);
	}

	const FMoQResult InitResult = Session->Initialize(Options.RelayUrl);
	if (!InitResult.IsOk())
	{
		UE_LOG(LogO3DMoQReceiver, Error, TEXT("Failed to initialize MoQ session: %s"), *InitResult.Message);
		return FO3DTransportResult::Error(EO3DTransportError::ResourceUnavailable, FString::Printf(TEXT("Failed to initialize the MoQ session: %s"), *InitResult.Message));
	}

	if (!ConnectionDelegateHandle.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQReceiver::HandleConnectionStateChanged);
	}

	ActiveConfig = Config;
	ActiveAudioConfig = Config.Audio;
	ResetStats();

	// The consumer gets Options.StreamId as its stream, as does the control sink (ADR 0011).
	FO3DReceiveDemuxSettings DemuxSettings = Demux.GetSettings();
	DemuxSettings.StreamId = Options.StreamId;
	Demux.SetSettings(DemuxSettings);
	Demux.ResetStats();

	// Drain any stale payloads
	ReceiveQueue.Empty();

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	bMocapSubscribed = false;
	bAudioSubscribed = false;
	bControlSubscribed = false;
	ConsecutiveFailures = 0;
	LastConnectAttemptTimeSeconds = 0.0;
	NextConnectAttemptTimeSeconds = 0.0;
	LastSubscribeAttemptTimeSeconds = 0.0;
	MocapSubscribeRetry.Reset();
	AudioSubscribeRetry.Reset();
	ControlSubscribeRetry.Reset();
	LastErrorLogTimeSeconds = 0.0;

	bInitialized = true;
	return FO3DTransportResult::Ok();
}

void FO3DMoQReceiver::SetAudioSink(const TSharedPtr<IO3DReceiverAudioSink, ESPMode::ThreadSafe>& Sink, const FO3DTransportAudioConfig& AudioConfig)
{
	Demux.SetAudioSink(Sink);
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

void FO3DMoQReceiver::SetControlSink(const TSharedPtr<IO3DReceiverControlSink, ESPMode::ThreadSafe>& Sink)
{
	Demux.SetControlSink(Sink);
	if (!Sink.IsValid())
	{
		DestroyControlSubscriber();
	}
	else if (CachedState.Load() == MOQ_STATE_CONNECTED && !bControlSubscribed)
	{
		AttemptControlSubscribe();
	}
}

FO3DTransportResult FO3DMoQReceiver::Start()
{
	if (!bInitialized)
	{
		UE_LOG(LogO3DMoQReceiver, Warning, TEXT("MoQ receiver Start called before Initialize"));
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("MoQ receiver Start() before a successful Initialize()."));
	}

	if (bRunning)
	{
		return FO3DTransportResult::Ok();
	}

	if (!Demux.HasConsumer())
	{
		return FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("MoQ receiver Start() without a frame consumer (SetConsumer)."));
	}

	if (!ConnectionDelegateHandle.IsValid() && Session.IsValid())
	{
		ConnectionDelegateHandle = Session->OnConnectionStateChanged().AddRaw(this, &FO3DMoQReceiver::HandleConnectionStateChanged);
	}

	bRunning = true;
	ConnectionState.Begin(EO3DConnectionState::Connecting);
	if (!AttemptConnect())
	{
		UE_LOG(LogO3DMoQReceiver, Error, TEXT("Initial MoQ connection attempt failed"));
		bRunning = false;
		const FO3DTransportResult Result = FO3DTransportResult::Error(EO3DTransportError::ConnectFailed,
			FString::Printf(TEXT("The first MoQ connection attempt to %s failed."), *O3DRedact::Url(Options.RelayUrl)));
		ConnectionState.End(EO3DConnectionState::Failed, Result);
		return Result;
	}

	return FO3DTransportResult::Ok();
}

void FO3DMoQReceiver::Stop()
{
	// The consumer and both sinks are released here, never called again after Stop (TRF-38).
	Demux.ReleaseSinks();

	if (!bInitialized && !bRunning)
	{
		ConnectionState.End(EO3DConnectionState::Idle);
		return;
	}

	bRunning = false;

	DestroySubscriber();
	DestroyAudioSubscriber();
	DestroyControlSubscriber();

	if (Session.IsValid())
	{
		if (ConnectionDelegateHandle.IsValid())
		{
			Session->OnConnectionStateChanged().Remove(ConnectionDelegateHandle);
			ConnectionDelegateHandle.Reset();
		}
		Session->Disconnect();
	}

	// Drain the receive queue; frames left unread count as dropped (control is never a frame).
	const int32 DrainedFrames = ReceiveQueue.Empty();
	{
		FScopeLock Lock(&StatsMutex);
		Stats.DroppedFrames += DrainedFrames;
	}

	CachedState = MOQ_STATE_DISCONNECTED;
	bConnectInFlight = false;
	bMocapSubscribed = false;
	bAudioSubscribed = false;
	bControlSubscribed = false;
	ConnectionState.End(EO3DConnectionState::Idle);
}

void FO3DMoQReceiver::ReportSessionLost(const FString& Reason)
{
	// Before the first connection the receiver stays Connecting while it retries.
	if (ConnectionState.Get() == EO3DConnectionState::Connected)
	{
		ConnectionState.Set(EO3DConnectionState::Reconnecting, FO3DTransportResult::Error(EO3DTransportError::ConnectFailed, Reason));
	}
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
	DestroyControlSubscriber();
	ScheduleReconnect(Now);
	ReportSessionLost(TEXT("The MoQ connect attempt timed out."));
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
		ControlSubscribeRetry.Reset();
		// Subscribe to mocap track
		AttemptSubscribe();
		// Subscribe to audio track if audio sink is configured
		if (Demux.HasAudioSink())
		{
			AttemptAudioSubscribe();
		}
		// Subscribe to the control track only while control is wanted (a sink is set)
		if (Demux.HasControlSink())
		{
			AttemptControlSubscribe();
		}
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
		DestroySubscriber();
		DestroyAudioSubscriber();
		DestroyControlSubscriber();
		bMocapSubscribed = false;
		bAudioSubscribed = false;
		ScheduleReconnect(NowSeconds());
		ReportSessionLost(NewState == MOQ_STATE_FAILED ? TEXT("The MoQ session failed.") : TEXT("The MoQ session disconnected."));
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

	// The sender announces mocap and control together on connect, so a control subscribe that is
	// backing off is retried now rather than leaving the first cues unheard.
	if (Demux.HasControlSink() && !bControlSubscribed)
	{
		AttemptControlSubscribe();
	}
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

bool FO3DMoQReceiver::AttemptControlSubscribe()
{
	if (!Session.IsValid() || !Session->IsConnected() || !Demux.HasControlSink())
	{
		return false;
	}

	if (bControlSubscribed && ControlSubscriberHandle.IsValid())
	{
		return true;
	}

	FMoQSubscriptionConfig SubscriptionConfig;
	SubscriptionConfig.Namespace = Options.ControlNamespace;
	SubscriptionConfig.TrackName = Options.TrackName;

	// Capture AliveFlag by value to safely handle callbacks
	TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AliveFlagCopy = AliveFlag;

	SubscriptionConfig.OnData = [this, AliveFlagCopy](const TArray64<uint8>& Payload)
	{
		if (!AliveFlagCopy.IsValid() || !(*AliveFlagCopy))
		{
			return;
		}
		HandleControlDataReceived(Payload);
	};

	TSharedPtr<FMoQSubscriberHandle> NewSubscriber;
	const FMoQResult Result = Session->Subscribe(SubscriptionConfig, NewSubscriber);
	if (!Result.IsOk())
	{
		// Verbose, with no shared throttle: a sender without control (an older build) never
		// announces the track, so this fails for as long as the stream runs.
		const double Now = NowSeconds();
		ScheduleSubscribeRetry(ControlSubscribeRetry, Now, /*SeedSalt=*/0x636F6E74726Full);
		UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("Failed to subscribe to control track %s/%s (retry in %.2f s): %s"),
			*Options.ControlNamespace, *Options.TrackName, ControlSubscribeRetry.NextAttemptTimeSeconds - Now, *Result.Message);
		return false;
	}

	ControlSubscribeRetry.Reset();
	ControlSubscriberHandle = NewSubscriber;
	bControlSubscribed = true;
	UE_LOG(LogO3DMoQReceiver, Log, TEXT("Subscribed to MoQ control track: %s/%s"), *Options.ControlNamespace, *Options.TrackName);
	return true;
}

bool FO3DMoQReceiver::EnqueueReceived(const TArray64<uint8>& Payload, EO3DSendItemKind Kind)
{
	if (Payload.Num() <= 0 || Payload.Num() > static_cast<int64>(MAX_int32))
	{
		return false;
	}

	TArray<uint8> Bytes(Payload.GetData(), static_cast<int32>(Payload.Num()));
	const double ReceiveTime = FPlatformTime::Seconds(); // the latency stats measure queue time
	FO3DSendItem Item;
	switch (Kind)
	{
	case EO3DSendItemKind::Audio:
		Item = FO3DSendItem::MakeAudio(MoveTemp(Bytes), FString(), ReceiveTime);
		break;
	case EO3DSendItemKind::Control:
		Item = FO3DSendItem::MakeControl(MoveTemp(Bytes));
		Item.CaptureTimeSec = ReceiveTime;
		break;
	default:
		Item = FO3DSendItem::MakeMocap(MoveTemp(Bytes), FString(), ReceiveTime);
		break;
	}
	return ReceiveQueue.Enqueue(MoveTemp(Item)) == EO3DSendResult::Queued;
}

void FO3DMoQReceiver::HandleMocapDataReceived(const TArray64<uint8>& Payload)
{
	if (!bRunning || Payload.IsEmpty())
	{
		return;
	}

	if (!EnqueueReceived(Payload, EO3DSendItemKind::Mocap))
	{
		UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("MoQ receiver queue overflow; dropping incoming mocap payload"));
		FScopeLock StatsLock(&StatsMutex);
		Stats.DroppedFrames++;
	}
}

void FO3DMoQReceiver::HandleAudioDataReceived(const TArray64<uint8>& Payload)
{
	if (!bRunning || Payload.IsEmpty())
	{
		return;
	}

	if (!EnqueueReceived(Payload, EO3DSendItemKind::Audio))
	{
		UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("MoQ receiver queue overflow; dropping incoming audio payload"));
		FScopeLock StatsLock(&StatsMutex);
		Stats.DroppedFrames++;
	}
}

void FO3DMoQReceiver::HandleControlDataReceived(const TArray64<uint8>& Payload)
{
	if (!bRunning || Payload.IsEmpty())
	{
		return;
	}

	// Never counted as a dropped frame; the control publisher's redundancy and snapshots repair it.
	if (!EnqueueReceived(Payload, EO3DSendItemKind::Control))
	{
		UE_LOG(LogO3DMoQReceiver, Verbose, TEXT("MoQ receiver queue overflow; dropping incoming control payload"));
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

void FO3DMoQReceiver::DestroyControlSubscriber()
{
	ControlSubscriberHandle.Reset();
	bControlSubscribed = false;
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
	if (State == MOQ_STATE_CONNECTED && !bAudioSubscribed && Demux.HasAudioSink() && Now >= AudioSubscribeRetry.NextAttemptTimeSeconds)
	{
		AttemptAudioSubscribe();
	}
	if (State == MOQ_STATE_CONNECTED && !bControlSubscribed && Demux.HasControlSink() && Now >= ControlSubscribeRetry.NextAttemptTimeSeconds)
	{
		AttemptControlSubscribe();
	}

	int32 FramesProcessed = 0;
	int32 ControlProcessed = 0;

	while (FramesProcessed < kMaxFramesPerPoll && ControlProcessed < kMaxControlPerPoll)
	{
		FO3DSendItem Item;
		if (!ReceiveQueue.Dequeue(Item))
		{
			break;
		}

		if (Item.Kind == EO3DSendItemKind::Control)
		{
			// Not a frame: never reaches the frame consumer and moves no frame counter (ADR 0011).
			++ControlProcessed;
		}
		else if (Item.Kind == EO3DSendItemKind::Audio)
		{
			++FramesProcessed; // bounds the work per Poll, as before; not counted as a received frame
		}
		else
		{
			++FramesProcessed;
		}
		RouteReceived(Item);
		if (!bRunning)
		{
			break; // a consumer or sink stopped this receiver
		}
	}

	return FramesProcessed;
}

bool FO3DMoQReceiver::RouteReceived(FO3DSendItem& Item)
{
	// One classification for every track (ADR 0007 item 7). The wire format per track is
	// unchanged: a bare O3DS frame, a bare audio payload, a control envelope.
	const int32 NumBytes = Item.Bytes.Num();
	EO3DDemuxResult Result = EO3DDemuxResult::Malformed;
	switch (Item.Kind)
	{
	case EO3DSendItemKind::Audio:
		Result = Demux.DeliverAudioPayload(Item.Bytes.GetData(), Item.Bytes.Num());
		break;
	case EO3DSendItemKind::Control:
		Result = Demux.DeliverControlEnvelope(Item.Bytes.GetData(), Item.Bytes.Num(), Item.CaptureTimeSec);
		break;
	default:
		// The queue item owns the frame, so the consumer gets it without a copy (WP-A1 PR 5b).
		Result = Demux.DeliverMocapOwned(FString(), MoveTemp(Item.Bytes), Item.CaptureTimeSec);
		break;
	}

	FScopeLock Lock(&StatsMutex);
	switch (Result)
	{
	case EO3DDemuxResult::Mocap:
	{
		Stats.FramesReceived++;
		Stats.BytesReceived += NumBytes;
		const double LatencyMs = (FPlatformTime::Seconds() - Item.CaptureTimeSec) * 1000.0;
		LatencyStats.TotalLatencyMs += LatencyMs;
		LatencyStats.Samples++;
		LatencyStats.MaxLatencyMs = FMath::Max(LatencyStats.MaxLatencyMs, LatencyMs);
		return true;
	}
	case EO3DDemuxResult::AudioRejected:
	case EO3DDemuxResult::Malformed:
	case EO3DDemuxResult::Oversize:
		Stats.ReceiveErrors++;
		return false;
	default:
		return false; // audio (to the sink), control (to the control sink)
	}
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
	Copy.State = ConnectionState.Get();
	return Copy;
}

void FO3DMoQReceiver::ResetStats()
{
	FScopeLock Lock(&StatsMutex);
	Stats.Reset();
	LatencyStats = FLatencyStats();
}

#endif // O3D_WITH_TRANSPORT_MOQ
