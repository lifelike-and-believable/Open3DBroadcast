// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DBlueprintTransportTypes.h"

#include "Misc/ScopeLock.h"

static_assert(static_cast<uint8>(EO3DBroadcastConnectionState::Idle) == static_cast<uint8>(EO3DConnectionState::Idle), "keep EO3DBroadcastConnectionState in step with EO3DConnectionState");
static_assert(static_cast<uint8>(EO3DBroadcastConnectionState::Connecting) == static_cast<uint8>(EO3DConnectionState::Connecting), "keep EO3DBroadcastConnectionState in step with EO3DConnectionState");
static_assert(static_cast<uint8>(EO3DBroadcastConnectionState::Connected) == static_cast<uint8>(EO3DConnectionState::Connected), "keep EO3DBroadcastConnectionState in step with EO3DConnectionState");
static_assert(static_cast<uint8>(EO3DBroadcastConnectionState::Reconnecting) == static_cast<uint8>(EO3DConnectionState::Reconnecting), "keep EO3DBroadcastConnectionState in step with EO3DConnectionState");
static_assert(static_cast<uint8>(EO3DBroadcastConnectionState::Failed) == static_cast<uint8>(EO3DConnectionState::Failed), "keep EO3DBroadcastConnectionState in step with EO3DConnectionState");

EO3DBroadcastConnectionState O3DBlueprintTypes::ToBlueprint(EO3DConnectionState State)
{
	return static_cast<EO3DBroadcastConnectionState>(State);
}

FO3DBroadcastTransportStats O3DBlueprintTypes::ToBlueprint(const FO3DTransportStats& Stats)
{
	FO3DBroadcastTransportStats Out;
	Out.State = ToBlueprint(Stats.State);
	Out.FramesSent = Stats.FramesSent;
	Out.BytesSent = Stats.BytesSent;
	Out.FramesReceived = Stats.FramesReceived;
	Out.BytesReceived = Stats.BytesReceived;
	Out.DroppedFrames = Stats.DroppedFrames;
	Out.SendErrors = Stats.SendErrors;
	Out.ReceiveErrors = Stats.ReceiveErrors;
	Out.PendingFrames = Stats.PendingFrames;
	Out.AverageLatencyMs = Stats.AverageLatencyMs;
	Out.MaxLatencyMs = Stats.MaxLatencyMs;
	return Out;
}

void FO3DConnectionStateMailbox::Post(EO3DConnectionState State, const FO3DTransportResult& Reason)
{
	FScopeLock ScopeLock(&Lock);
	if (Pending.Num() >= MaxPending)
	{
		Pending.RemoveAt(0);
	}
	Pending.Add(FChange{ State, Reason });
}

TArray<FO3DConnectionStateMailbox::FChange> FO3DConnectionStateMailbox::Drain()
{
	FScopeLock ScopeLock(&Lock);
	return MoveTemp(Pending);
}

void FO3DConnectionStateMailbox::Reset()
{
	FScopeLock ScopeLock(&Lock);
	Pending.Reset();
}

FO3DConnectionStateCallback FO3DConnectionStateMailbox::MakeCallback(const TSharedRef<FO3DConnectionStateMailbox, ESPMode::ThreadSafe>& Mailbox)
{
	return [Mailbox](EO3DConnectionState State, const FO3DTransportResult& Reason)
	{
		Mailbox->Post(State, Reason);
	};
}
