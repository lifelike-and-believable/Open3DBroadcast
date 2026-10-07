// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/MoQHelpers.h"
#include "Math/UnrealMathUtility.h"
#include "Transport/O3DTransportOptions.h"

namespace MoQHelpers
{
	FO3DTransportCapabilities GetCapabilities(const FO3DTransportConfig& /*Config*/)
	{
		FO3DTransportCapabilities Caps;
		Caps.bSend = true;
		Caps.bReceive = true;
		Caps.bAudioSend = true;
		Caps.bAudioReceive = true;
		Caps.bControl = true;
		Caps.Delivery = EO3DDeliveryGuarantee::Unreliable;
		return Caps;
	}

	/** The trimmed value of Key (case-insensitive), or empty (O3DTransportOptions, WP-A1 PR 4e). */
	static FString GetMoQOption(const FO3DTransportConfig& Config, const TCHAR* Key)
	{
		return O3DTransportOptions::GetString(Config.AdvancedParams, Key);
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
		
		// Apply length limit
		if (bAllowSlash && Result.Len() > kMaxNamespaceLength)
		{
			Result.LeftInline(kMaxNamespaceLength);
		}
		else if (!bAllowSlash && Result.Len() > kMaxTrackNameLength)
		{
			Result.LeftInline(kMaxTrackNameLength);
		}
		
		return Result;
	}

	static FString BuildNamespaceWithPrefix(const FO3DTransportConfig& Config, const TCHAR* Prefix)
	{
		FString Namespace = GetMoQOption(Config, kKeyTrackNamespace);
		if (Namespace.IsEmpty())
		{
			Namespace = GetMoQOption(Config, kKeyTrackNamespaceAlt);
		}

		if (Namespace.IsEmpty())
		{
			FString SessionId = GetMoQOption(Config, kKeySessionId);
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
				SessionId = kDefaultSessionName;
			}

			SessionId = SanitizeComponent(SessionId, false);
			Namespace = FString::Printf(TEXT("%s/%s"), Prefix, *SessionId);
		}
		else
		{
			// Replace the prefix if the namespace starts with mocap/ or audio/
			// Calculate prefix lengths dynamically to avoid magic numbers
			static const int32 MocapPrefixLen = FCString::Strlen(kMocapNamespacePrefix) + 1; // +1 for slash
			static const int32 AudioPrefixLen = FCString::Strlen(kAudioNamespacePrefix) + 1; // +1 for slash
			
			if (Namespace.StartsWith(TEXT("mocap/"), ESearchCase::IgnoreCase))
			{
				Namespace = FString::Printf(TEXT("%s/%s"), Prefix, *Namespace.Mid(MocapPrefixLen));
			}
			else if (Namespace.StartsWith(TEXT("audio/"), ESearchCase::IgnoreCase))
			{
				Namespace = FString::Printf(TEXT("%s/%s"), Prefix, *Namespace.Mid(AudioPrefixLen));
			}
		}

		Namespace = SanitizeComponent(Namespace, true);
		if (Namespace.EndsWith(TEXT("/")))
		{
			Namespace.LeftChopInline(1);
		}

		return Namespace;
	}

	FString BuildDefaultMocapNamespace(const FO3DTransportConfig& Config)
	{
		return BuildNamespaceWithPrefix(Config, kMocapNamespacePrefix);
	}

	FString BuildDefaultAudioNamespace(const FO3DTransportConfig& Config)
	{
		const FString Namespace = BuildNamespaceWithPrefix(Config, kAudioNamespacePrefix);
		const FString Mocap = BuildDefaultMocapNamespace(Config);
		if (Namespace != Mocap)
		{
			return Namespace;
		}
		// A custom namespace the prefix swap leaves unchanged: audio must not share the mocap track.
		return SanitizeComponent(FString::Printf(TEXT("%s/%s"), kAudioNamespacePrefix, *Mocap), true);
	}

	FString BuildDefaultControlNamespace(const FO3DTransportConfig& Config)
	{
		const FString Namespace = BuildNamespaceWithPrefix(Config, kControlNamespacePrefix);
		const FString Mocap = BuildDefaultMocapNamespace(Config);
		if (Namespace != Mocap)
		{
			return Namespace;
		}
		// A custom namespace the prefix swap leaves unchanged: control must not ride the mocap track.
		return SanitizeComponent(FString::Printf(TEXT("%s/%s"), kControlNamespacePrefix, *Mocap), true);
	}

	FString BuildDefaultTrackName(const FO3DTransportConfig& Config)
	{
		FString TrackName = GetMoQOption(Config, kKeyTrackName);
		if (TrackName.IsEmpty())
		{
			TrackName = GetMoQOption(Config, kKeyTrackNameAlt);
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
			TrackName = kDefaultTrackName;
		}

		TrackName = SanitizeComponent(TrackName, false);
		if (TrackName.IsEmpty())
		{
			TrackName = kDefaultTrackName;
		}

		return TrackName;
	}

	FString ResolveRelayUrl(const FO3DTransportConfig& Config)
	{
		FString Relay = GetMoQOption(Config, kKeyRelayUrl);
		if (Relay.IsEmpty())
		{
			Relay = GetMoQOption(Config, kKeyRelayUrlAlt);
		}
		if (Relay.IsEmpty())
		{
			Relay = Config.Uri;
		}

		Relay.TrimStartAndEndInline();
		return Relay;
	}

	MoqDeliveryMode ResolveDeliveryMode(const FO3DTransportConfig& Config)
	{
		FString Mode = GetMoQOption(Config, kKeyDeliveryMode);
		if (Mode.IsEmpty())
		{
			Mode = GetMoQOption(Config, kKeyDeliveryModeAlt);
		}

		if (Mode.Equals(TEXT("datagram"), ESearchCase::IgnoreCase))
		{
			return MOQ_DELIVERY_DATAGRAM;
		}

		return MOQ_DELIVERY_STREAM;
	}

	uint64 ResolveQueueBytes(const FO3DTransportConfig& Config)
	{
		uint64 QueueBytes = kDefaultQueueBytes;
		FString QueueOverride = GetMoQOption(Config, kKeyQueueBytes);
		if (QueueOverride.IsEmpty())
		{
			QueueOverride = GetMoQOption(Config, kKeyQueueBytesAlt);
		}
		if (QueueOverride.IsEmpty())
		{
			QueueOverride = GetMoQOption(Config, kKeyQueueBytesAlt2);
		}

		int64 Parsed = 0;
		if (!QueueOverride.IsEmpty() && O3DTransportOptions::TryParseInt(QueueOverride, Parsed) && Parsed >= 0)
		{
			QueueBytes = static_cast<uint64>(Parsed);
		}

		QueueBytes = FMath::Clamp<uint64>(QueueBytes, kMinQueueBytes, kMaxQueueBytes);
		return QueueBytes;
	}

	double ComputeReconnectDelaySeconds(int32 ConsecutiveFailures)
	{
		const int32 Attempts = FMath::Clamp(ConsecutiveFailures, 0, 6);
		const double Delay = 0.5 * FMath::Pow(2.0, static_cast<double>(Attempts));
		return FMath::Clamp(Delay, kMinReconnectDelaySeconds, kMaxReconnectDelaySeconds);
	}

	double ComputeJitterUnit(uint64 JitterSeed, int32 ConsecutiveFailures)
	{
		// splitmix64 finaliser: cheap, stateless and well mixed.
		uint64 X = JitterSeed + 0x9E3779B97F4A7C15ull * (static_cast<uint64>(FMath::Max(ConsecutiveFailures, 0)) + 1ull);
		X = (X ^ (X >> 30)) * 0xBF58476D1CE4E5B9ull;
		X = (X ^ (X >> 27)) * 0x94D049BB133111EBull;
		X = X ^ (X >> 31);
		// Top 53 bits give a double in [0, 1).
		return static_cast<double>(X >> 11) * (1.0 / 9007199254740992.0);
	}

	double ComputeBackoffDelaySeconds(int32 ConsecutiveFailures, uint64 JitterSeed)
	{
		const double Base = ComputeReconnectDelaySeconds(ConsecutiveFailures);
		const double Unit = ComputeJitterUnit(JitterSeed, ConsecutiveFailures);
		return Base * (1.0 - kBackoffJitterFraction * Unit);
	}

	double ResolveConnectTimeoutSeconds(const FO3DTransportConfig& Config)
	{
		FString Value = GetMoQOption(Config, kKeyConnectTimeout);
		if (Value.IsEmpty())
		{
			Value = GetMoQOption(Config, kKeyConnectTimeoutAlt);
		}

		double Seconds = kDefaultConnectTimeoutSeconds;
		double Parsed = 0.0;
		if (!Value.IsEmpty() && O3DTransportOptions::TryParseDouble(Value, Parsed))
		{
			Seconds = Parsed;
		}

		return FMath::Clamp(Seconds, kMinConnectTimeoutSeconds, kMaxConnectTimeoutSeconds);
	}
}

#endif // O3D_WITH_TRANSPORT_MOQ
