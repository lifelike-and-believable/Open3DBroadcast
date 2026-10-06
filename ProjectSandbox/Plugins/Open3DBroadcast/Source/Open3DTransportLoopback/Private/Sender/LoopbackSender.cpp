// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "LoopbackSender.h"
#include "O3DPerformanceMetrics.h"

#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "O3DUnifiedMessage.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END


FO3DLoopbackSender::~FO3DLoopbackSender()
{
	if (PublishState.IsValid())
	{
		PublishState->Close();
	}
}

FO3DTransportResult FO3DLoopbackSender::Initialize(const FO3DTransportConfig& Config)
{
	// A sink of the previous session must never reach the new channel.
	if (PublishState.IsValid())
	{
		PublishState->Close();
	}

	ChannelKey = O3DLoopback::ResolveChannelKey(Config);
	const FO3DSendQueueLimits Limits = O3DLoopback::ResolveQueueLimits(Config);
	const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue = O3DLoopback::AcquireChannel(ChannelKey, &Limits);
	Channel = Queue;
	PublishState = MakeShared<FO3DAudioPublishState, ESPMode::ThreadSafe>(Queue, EO3DAudioWireFormat::UnifiedEnvelope);
	// Open now, so a sink created between Initialize and Start is bound to this session.
	PublishState->Open();
	bInitialized = true;

	SenderMetrics = Config.SenderMetrics;
	FramesSent.store(0);
	BytesSent.store(0);
	DroppedFrames.store(0);
	ActiveAudioConfig = Config.Audio;
	AudioSourceGuid = FGuid::NewGuid();

	if (O3DLoopback::GetAudioDebugLevel() > 0)
	{
		UE_LOG(LogO3DLoopbackTransport, Log, TEXT("Loopback sender initialized channel='%s' queueCapacity=%d audioCapacity=%d"),
			*ChannelKey, Limits.Mocap.MaxItems, Limits.Audio.MaxItems);
	}
	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DLoopbackSender::Start()
{
	if (!bInitialized)
	{
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("Loopback sender Start() before a successful Initialize()."));
	}
	PublishState->Open();
	bRunning.store(true);
	// The in-process channel needs no peer, so a started sender can deliver at once.
	ConnectionState.Begin(EO3DConnectionState::Connected);
	return FO3DTransportResult::Ok();
}

void FO3DLoopbackSender::Stop()
{
	// WP-S5: after this returns no audio sink created so far can enqueue again. The channel stays
	// available to its receiver and to new senders.
	if (PublishState.IsValid())
	{
		PublishState->Close();
	}
	bRunning.store(false);
	ConnectionState.End(EO3DConnectionState::Idle);
}

EO3DSendResult FO3DLoopbackSender::EnqueueFrame(TArray<uint8>&& Bytes, FString SubjectName, double CaptureTimeSec, bool bFullSync)
{
	const int64 Len = Bytes.Num();
	const FString Subject = SubjectName.IsEmpty() ? ChannelKey : MoveTemp(SubjectName);
	const EO3DSendResult Result = Channel->Enqueue(FO3DSendItem::MakeMocap(MoveTemp(Bytes), Subject, CaptureTimeSec, bFullSync));
	if (Result == EO3DSendResult::Queued)
	{
		// Audio frames carry the subject last sent (their metadata's SubjectName).
		PublishState->GetSubjectSlot().Set(Subject);
		FramesSent.fetch_add(1);
		BytesSent.fetch_add(Len);
		if (SenderMetrics.IsValid())
		{
			SenderMetrics->RecordBytesSent(static_cast<uint64>(Len)); // WP-R3
		}
	}
	else if (Result == EO3DSendResult::DroppedBackpressure)
	{
		DroppedFrames.fetch_add(1);
		// WP-U6 (TRB-32): a full channel usually means nothing reads it, so users should see it.
		int64 Suppressed = 0;
		if (QueueFullLog.ShouldLog(Suppressed))
		{
			UE_LOG(LogO3DLoopbackTransport, Warning, TEXT("Loopback queue full for '%s': no receiver is reading it fast enough, so frames are dropped (%lld more since the last warning)."), *ChannelKey, Suppressed);
		}
	}
	return Result;
}

EO3DSendResult FO3DLoopbackSender::SendSerialized(FO3DSendPayload&& Payload)
{
	if (!bInitialized || !bRunning.load() || !Channel.IsValid())
	{
		return EO3DSendResult::NotRunning;
	}
	if (Payload.Bytes.Num() <= 0)
	{
		return EO3DSendResult::Invalid;
	}
	return EnqueueFrame(MoveTemp(Payload.Bytes), MoveTemp(Payload.Subject), Payload.CaptureTimeSec, Payload.bFullSync);
}

/** Control (ADR 0011): a control item on the shared queue, with its own cap; never counted as a frame. */
EO3DSendResult FO3DLoopbackSender::SendControl(const uint8* Envelope, int32 Len)
{
	if (!bInitialized || !bRunning.load() || !Channel.IsValid())
	{
		return EO3DSendResult::NotRunning;
	}
	TConstArrayView<uint8> Payload;
	if (!O3DS::TryGetControlPayload(Envelope, Len, Payload))
	{
		return EO3DSendResult::Invalid;
	}
	return Channel->Enqueue(FO3DSendItem::MakeControl(TArray<uint8>(Envelope, Len)));
}

FO3DTransportStats FO3DLoopbackSender::GetStats() const
{
	FO3DTransportStats Copy;
	Copy.FramesSent = FramesSent.load();
	Copy.BytesSent = BytesSent.load();
	Copy.DroppedFrames = DroppedFrames.load();
	Copy.State = ConnectionState.Get();
	if (Channel.IsValid())
	{
		Copy.PendingFrames = Channel->GetPendingItems(EO3DSendItemKind::Mocap);
		Copy.PendingBytes = Channel->GetPendingBytes();
	}
	return Copy;
}

TSharedPtr<IO3DSenderAudioSink, ESPMode::ThreadSafe> FO3DLoopbackSender::CreateAudioSink(const FO3DTransportAudioConfig& AudioConfig)
{
	if (!bInitialized || !PublishState.IsValid())
	{
		return nullptr;
	}

	ActiveAudioConfig = AudioConfig;

	// Immutable snapshot for this sink's own encoders (TRB-11): nothing reconfigures them later.
	FO3DSinkAudioEncoder::FSettings EncoderSettings;
	EncoderSettings.Config = ActiveAudioConfig;
	EncoderSettings.DefaultStreamLabel = ChannelKey;
	EncoderSettings.DefaultSubject = ChannelKey;
	EncoderSettings.SourceGuid = AudioSourceGuid;

	return MakeShared<FO3DQueuedSenderAudioSink, ESPMode::ThreadSafe>(PublishState.ToSharedRef(), ActiveAudioConfig, MoveTemp(EncoderSettings));
}
