// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DSendQueue.h"
#include "Transport/O3DTransportTypes.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DLoopbackTransport, Log, All);

/**
 * The loopback transport's in-process channel is one shared FO3DSendQueue per channel name
 * (ADR 0007 item 7, WP-A1 step 4). Senders enqueue mocap, audio and control items; the receiver's
 * Poll is the queue's single consumer and hands each item to its FO3DUnifiedReceiveDemux. Each
 * kind has its own limit (ADR 0011: the three kinds stay independent), mocap refuses the newest
 * frame when full (Loopback is ReliableOrdered), and nothing queued is ever discarded.
 */
namespace O3DLoopback
{
	/** Option keys, as persisted in component and source settings. */
	inline constexpr TCHAR ChannelOptionKey[] = TEXT("channel");
	inline constexpr TCHAR QueueOptionKey[] = TEXT("loopback.maxqueue");
	inline constexpr TCHAR AudioQueueOptionKey[] = TEXT("loopback.maxaudioqueue");
	inline constexpr TCHAR DefaultChannel[] = TEXT("default");
	inline constexpr int32 DefaultQueueCapacity = 64;
	inline constexpr int32 DefaultAudioQueueCapacity = 32;

	/** The canonical channel key of a config: StreamId, else Uri, else "loopback"; trimmed and lowercased. */
	FString ResolveChannelKey(const FO3DTransportConfig& Config);

	/** Queue limits from the config's options (loopback.maxqueue, loopback.maxaudioqueue; legacy maxqueue, maxaudioqueue). */
	FO3DSendQueueLimits ResolveQueueLimits(const FO3DTransportConfig& Config);

	/**
	 * The queue of ChannelKey, created on first use. A sender passes its limits, which apply to the
	 * channel; a receiver passes nullptr and leaves them alone, so the sender's queue capacity holds
	 * whichever end starts first (WP-U6, TRB-31). A channel a receiver creates starts with the
	 * default limits until a sender sets its own. A channel lives as long as a sender or receiver
	 * holds it; channels nobody holds are pruned here.
	 */
	TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> AcquireChannel(const FString& ChannelKey, const FO3DSendQueueLimits* SenderLimits);

	/** The o3ds.Loopback.Audio.Debug console variable (0 off, 1 basic, 2 verbose). */
	int32 GetAudioDebugLevel();

	/** Capabilities of the loopback transport (ADR 0007 item 4); the same for every config. Any thread. */
	FO3DTransportCapabilities GetCapabilities(const FO3DTransportConfig& Config);
}
