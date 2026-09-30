// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/MoQHelpers.h"
#include "Math/UnrealMathUtility.h"

namespace MoQHelpers
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

	bool ParseUInt64(const FString& Input, uint64& OutValue)
	{
		if (Input.IsEmpty())
		{
			return false;
		}

		TCHAR* EndPtr = nullptr;
		OutValue = FCString::Strtoui64(*Input, &EndPtr, 10);
		return EndPtr != nullptr && *EndPtr == TEXT('\0');
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
		FString Namespace = GetAdvancedOption(Config, kKeyTrackNamespace);
		if (Namespace.IsEmpty())
		{
			Namespace = GetAdvancedOption(Config, kKeyTrackNamespaceAlt);
		}

		if (Namespace.IsEmpty())
		{
			FString SessionId = GetAdvancedOption(Config, kKeySessionId);
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
		return BuildNamespaceWithPrefix(Config, kAudioNamespacePrefix);
	}

	FString BuildDefaultTrackName(const FO3DTransportConfig& Config)
	{
		FString TrackName = GetAdvancedOption(Config, kKeyTrackName);
		if (TrackName.IsEmpty())
		{
			TrackName = GetAdvancedOption(Config, kKeyTrackNameAlt);
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
		FString Relay = GetAdvancedOption(Config, kKeyRelayUrl);
		if (Relay.IsEmpty())
		{
			Relay = GetAdvancedOption(Config, kKeyRelayUrlAlt);
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
		FString Mode = GetAdvancedOption(Config, kKeyDeliveryMode);
		if (Mode.IsEmpty())
		{
			Mode = GetAdvancedOption(Config, kKeyDeliveryModeAlt);
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
		FString QueueOverride = GetAdvancedOption(Config, kKeyQueueBytes);
		if (QueueOverride.IsEmpty())
		{
			QueueOverride = GetAdvancedOption(Config, kKeyQueueBytesAlt);
		}
		if (QueueOverride.IsEmpty())
		{
			QueueOverride = GetAdvancedOption(Config, kKeyQueueBytesAlt2);
		}

		uint64 Parsed = 0;
		if (!QueueOverride.IsEmpty() && ParseUInt64(QueueOverride, Parsed))
		{
			QueueBytes = Parsed;
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
		FString Value = GetAdvancedOption(Config, kKeyConnectTimeout);
		if (Value.IsEmpty())
		{
			Value = GetAdvancedOption(Config, kKeyConnectTimeoutAlt);
		}

		double Seconds = kDefaultConnectTimeoutSeconds;
		if (!Value.IsEmpty() && Value.IsNumeric())
		{
			Seconds = FCString::Atod(*Value);
		}

		return FMath::Clamp(Seconds, kMinConnectTimeoutSeconds, kMaxConnectTimeoutSeconds);
	}

	bool TryGetAudioCodecFromFrame(const uint8* Payload, int32 PayloadSize, O3DS::EUnifiedCodec& OutCodec)
	{
		// Mirrors the private constants in Open3DShared's O3DAudioSerialization.cpp
		// (AudioPayloadVersion = 1, EncodedAudioPayloadVersion = 2).
		constexpr uint8 Pcm16FrameVersion = 1;
		constexpr uint8 EncodedFrameVersion = 2;
		constexpr int32 EncodedCodecOffset = 2;

		if (Payload == nullptr || PayloadSize <= 0)
		{
			return false;
		}

		if (Payload[0] == Pcm16FrameVersion)
		{
			OutCodec = O3DS::EUnifiedCodec::PCM16;
			return true;
		}

		if (Payload[0] == EncodedFrameVersion && PayloadSize > EncodedCodecOffset)
		{
			// PCM16 is always written as a version 1 frame, so version 2 carries Opus today.
			if (Payload[EncodedCodecOffset] == static_cast<uint8>(O3DS::EUnifiedCodec::Opus))
			{
				OutCodec = O3DS::EUnifiedCodec::Opus;
				return true;
			}
		}

		return false;
	}
}

#endif // O3D_WITH_TRANSPORT_MOQ
