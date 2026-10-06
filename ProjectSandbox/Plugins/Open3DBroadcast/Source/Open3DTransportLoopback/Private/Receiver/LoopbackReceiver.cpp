// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "LoopbackReceiver.h"

#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Logging/LogMacros.h"

FO3DTransportResult FO3DLoopbackReceiver::Initialize(const FO3DTransportConfig& Config)
{
	ChannelKey = O3DLoopback::ResolveChannelKey(Config);
	Channel = O3DLoopback::AcquireChannel(ChannelKey, O3DLoopback::ResolveQueueLimits(Config));
	bInitialized = true;

	FO3DReceiveDemuxSettings Settings = Demux.GetSettings();
	Settings.StreamId = ChannelKey;
	Demux.SetSettings(Settings);
	Demux.ResetStats();

	{
		FScopeLock Lock(&StatsMutex);
		Stats.Reset();
		LatencySamples = 0;
	}
	return FO3DTransportResult::Ok();
}

FO3DTransportResult FO3DLoopbackReceiver::Start()
{
	if (!bInitialized)
	{
		return FO3DTransportResult::Error(EO3DTransportError::NotRunning, TEXT("Loopback receiver Start() before a successful Initialize()."));
	}
	// ADR 0007 item 3: a receiver without a consumer refuses to start (SHR-24).
	if (!Demux.HasConsumer())
	{
		return FO3DTransportResult::Error(EO3DTransportError::NoConsumer, TEXT("Loopback receiver Start() without a frame consumer (SetConsumer)."));
	}
	ConnectionState.Begin(EO3DConnectionState::Connected);
	return FO3DTransportResult::Ok();
}

void FO3DLoopbackReceiver::Stop()
{
	Demux.ReleaseSinks();
	ConnectionState.End(EO3DConnectionState::Idle);
}

int32 FO3DLoopbackReceiver::Poll()
{
	if (!bInitialized || !Channel.IsValid())
	{
		return 0;
	}

	const int32 DebugLevel = O3DLoopback::GetAudioDebugLevel();
	int32 Processed = 0;
	FO3DSendItem Item;
	while (Channel->Dequeue(Item))
	{
		// LiveLink expects "when to display", so frames are stamped on arrival (as every transport does).
		const double NowSeconds = FPlatformTime::Seconds();
		switch (Item.Kind)
		{
		case EO3DSendItemKind::Mocap:
			++Processed;
			{
				FScopeLock Lock(&StatsMutex);
				Stats.FramesReceived++;
				Stats.BytesReceived += Item.Bytes.Num();
				AccumulateLatency((NowSeconds - Item.CaptureTimeSec) * 1000.0);
			}
			// The channel item owns the frame, so the consumer gets it without a copy (WP-A1 PR 5b).
			Demux.DeliverMocapOwned(Item.Subject, MoveTemp(Item.Bytes), NowSeconds);
			break;

		case EO3DSendItemKind::Audio:
		{
			++Processed;
			const EO3DDemuxResult Result = Demux.ProcessMessage(Item.Bytes.GetData(), Item.Bytes.Num(), NowSeconds);
			{
				FScopeLock Lock(&StatsMutex);
				Stats.BytesReceived += Item.Bytes.Num();
				if (Result == EO3DDemuxResult::AudioRejected)
				{
					Stats.ReceiveErrors++;
				}
			}
			if (Result == EO3DDemuxResult::AudioRejected)
			{
				UE_LOG(LogO3DLoopbackTransport, Verbose, TEXT("Loopback audio frame rejected on '%s'."), *ChannelKey);
			}
			else if (DebugLevel > 0 && (DebugLevel > 1 || NowSeconds - LastAudioLogTime > 0.25))
			{
				UE_LOG(LogO3DLoopbackTransport, Log, TEXT("Loopback audio dequeued channel='%s' bytes=%d result=%s pending=%d"),
					*ChannelKey, Item.Bytes.Num(), LexToString(Result), Channel->GetPendingItems(EO3DSendItemKind::Audio));
				LastAudioLogTime = NowSeconds;
			}
			break;
		}

		case EO3DSendItemKind::Control:
			// ADR 0011: to the control sink; not counted as a frame.
			Demux.ProcessMessage(Item.Bytes.GetData(), Item.Bytes.Num(), NowSeconds);
			break;
		}
	}
	return Processed;
}

FO3DTransportStats FO3DLoopbackReceiver::GetStats() const
{
	FO3DTransportStats Copy;
	{
		FScopeLock Lock(&StatsMutex);
		Copy = Stats;
	}
	Copy.State = ConnectionState.Get();
	return Copy;
}

void FO3DLoopbackReceiver::AccumulateLatency(double LatencyMs)
{
	Stats.MaxLatencyMs = FMath::Max(Stats.MaxLatencyMs, LatencyMs);

	const int64 NewSampleCount = LatencySamples + 1;
	const double PreviousTotal = Stats.AverageLatencyMs * LatencySamples;
	Stats.AverageLatencyMs = (PreviousTotal + LatencyMs) / FMath::Max<int64>(1, NewSampleCount);
	LatencySamples = NewSampleCount;
}
