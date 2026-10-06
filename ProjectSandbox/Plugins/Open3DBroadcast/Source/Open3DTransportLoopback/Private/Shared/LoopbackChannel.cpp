// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "LoopbackChannel.h"

#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"
#include "Transport/O3DTransportOptions.h"

DEFINE_LOG_CATEGORY(LogO3DLoopbackTransport);

namespace O3DLoopbackChannelPrivate
{
	FCriticalSection GChannelMutex;
	TMap<FString, TWeakPtr<FO3DSendQueue, ESPMode::ThreadSafe>> GChannels;

	FString NormaliseChannelKey(const FString& InKey)
	{
		FString Key = InKey.TrimStartAndEnd();
		if (Key.IsEmpty())
		{
			Key = O3DLoopback::DefaultChannel;
		}
		return Key.ToLower();
	}

	/** A positive capacity from Key or its legacy alias; Default when absent, not a number, or below 1. */
	int32 ReadCapacity(const FO3DTransportConfig& Config, const TCHAR* Key, const TCHAR* LegacyKey, int32 Default)
	{
		const TCHAR* UsedKey = O3DTransportOptions::Find(Config.AdvancedParams, Key) ? Key : LegacyKey;
		const int32 Value = O3DTransportOptions::GetInt(Config.AdvancedParams, UsedKey, Default);
		return Value > 0 ? Value : Default;
	}

	TAutoConsoleVariable<int32> CVarAudioDebug(
		TEXT("o3ds.Loopback.Audio.Debug"),
		0,
		TEXT("Enable verbose logging for the loopback transport audio path (0 = off, 1 = basic, 2 = verbose)."),
		ECVF_Default);
}

namespace O3DLoopback
{
	int32 GetAudioDebugLevel()
	{
		return O3DLoopbackChannelPrivate::CVarAudioDebug.GetValueOnAnyThread();
	}

	FO3DTransportCapabilities GetCapabilities(const FO3DTransportConfig& /*Config*/)
	{
		// In-process queue: nothing is lost or reordered except by a full queue, which the
		// sender reports as DroppedBackpressure (ADR 0005 (iii): Loopback is ReliableOrdered).
		FO3DTransportCapabilities Caps;
		Caps.bSend = true;
		Caps.bReceive = true;
		Caps.bAudioSend = true;
		Caps.bAudioReceive = true;
		Caps.bControl = true;
		Caps.Delivery = EO3DDeliveryGuarantee::ReliableOrdered;
		return Caps;
	}

	FO3DSendQueueLimits ResolveQueueLimits(const FO3DTransportConfig& Config)
	{
		FO3DSendQueueLimits Limits;
		Limits.Mocap.MaxItems = O3DLoopbackChannelPrivate::ReadCapacity(Config, QueueOptionKey, TEXT("maxqueue"), DefaultQueueCapacity);
		Limits.Audio.MaxItems = O3DLoopbackChannelPrivate::ReadCapacity(Config, AudioQueueOptionKey, TEXT("maxaudioqueue"), DefaultAudioQueueCapacity);
		// Control keeps the queue's default cap of its own (O3DSendQueueDefaultMaxControlItems).
		// ReliableOrdered: a full channel refuses the newest frame and never discards a queued one.
		Limits.MocapOverflow = EO3DMocapOverflow::RefuseNewest;
		return Limits;
	}

	FString ResolveChannelKey(const FO3DTransportConfig& Config)
	{
		using O3DLoopbackChannelPrivate::NormaliseChannelKey;
		if (!Config.StreamId.IsEmpty())
		{
			return NormaliseChannelKey(Config.StreamId);
		}
		if (!Config.Uri.IsEmpty())
		{
			return NormaliseChannelKey(Config.Uri);
		}
		return NormaliseChannelKey(TEXT("loopback"));
	}

	TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> AcquireChannel(const FString& ChannelKey, const FO3DSendQueueLimits& Limits)
	{
		using namespace O3DLoopbackChannelPrivate;
		const FString Key = NormaliseChannelKey(ChannelKey);
		FScopeLock Guard(&GChannelMutex);

		if (const TWeakPtr<FO3DSendQueue, ESPMode::ThreadSafe>* Existing = GChannels.Find(Key))
		{
			if (const TSharedPtr<FO3DSendQueue, ESPMode::ThreadSafe> Pinned = Existing->Pin())
			{
				Pinned->SetLimits(Limits);
				return Pinned.ToSharedRef();
			}
		}

		const TSharedRef<FO3DSendQueue, ESPMode::ThreadSafe> Queue = MakeShared<FO3DSendQueue, ESPMode::ThreadSafe>(Limits);
		GChannels.Add(Key, Queue);
		return Queue;
	}
}
