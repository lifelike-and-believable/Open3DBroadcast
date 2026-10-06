// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Transport/O3DTransportTypes.h"
#include "O3DBlueprintTransportTypes.generated.h"

/**
 * Blueprint view of a sender's or receiver's connection state (WP-U2; SND-26, UX-3). Mirrors
 * EO3DConnectionState, which belongs to the transport API and is not a reflected type.
 */
UENUM(BlueprintType)
enum class EO3DBroadcastConnectionState : uint8
{
	/** Not started, or stopped. */
	Idle,
	/** Started and waiting for a first peer, session or connection. */
	Connecting,
	/** Able to deliver. */
	Connected,
	/** Was connected, lost it, and is retrying. */
	Reconnecting,
	/** The transport could not start or gave up. Stop and start capture again to retry. */
	Failed,
};

/** Blueprint view of a transport's counters (FO3DTransportStats). */
USTRUCT(BlueprintType)
struct OPEN3DSHARED_API FO3DBroadcastTransportStats
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	EO3DBroadcastConnectionState State = EO3DBroadcastConnectionState::Idle;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 FramesSent = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 BytesSent = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 FramesReceived = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 BytesReceived = 0;

	/** Frames dropped because the transport could not take them. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 DroppedFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 SendErrors = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 ReceiveErrors = 0;

	/** Items waiting in the transport's send queue. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	int64 PendingFrames = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	double AverageLatencyMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast")
	double MaxLatencyMs = 0.0;
};

namespace O3DBlueprintTypes
{
	OPEN3DSHARED_API EO3DBroadcastConnectionState ToBlueprint(EO3DConnectionState State);
	OPEN3DSHARED_API FO3DBroadcastTransportStats ToBlueprint(const FO3DTransportStats& Stats);
}

/**
 * Carries a transport's connection-state changes from the thread that made them to the game
 * thread, where a component broadcasts them to Blueprint (WP-U2). The transport's state callback
 * (any thread) posts every change in order; the game thread drains them. Nothing is lost between
 * two drains, so a brief Failed between two ticks is still seen, with its reason. Owned through a
 * shared pointer, so a callback that outlives its component posts into the mailbox, not into the
 * component. Thread-safe.
 */
class OPEN3DSHARED_API FO3DConnectionStateMailbox
{
public:
	struct FChange
	{
		EO3DConnectionState State = EO3DConnectionState::Idle;
		FO3DTransportResult Reason;
	};

	/** Adds a change. Any thread. Keeps the latest MaxPending changes when nobody drains. */
	void Post(EO3DConnectionState State, const FO3DTransportResult& Reason);

	/** The changes posted since the last drain, oldest first. */
	TArray<FChange> Drain();

	/** Forgets pending changes. */
	void Reset();

	/** A callback for IOpen3DSender/IOpen3DReceiver::SetStateChangedCallback that posts into Mailbox. */
	static FO3DConnectionStateCallback MakeCallback(const TSharedRef<FO3DConnectionStateMailbox, ESPMode::ThreadSafe>& Mailbox);

	static constexpr int32 MaxPending = 64;

private:
	FCriticalSection Lock;
	TArray<FChange> Pending;
};
