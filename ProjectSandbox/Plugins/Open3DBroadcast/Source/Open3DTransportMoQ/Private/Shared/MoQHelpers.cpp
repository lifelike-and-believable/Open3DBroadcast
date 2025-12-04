// Copyright (c) Open3DStream Contributors

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
			// "mocap/" is 6 characters, so Mid(6) gets everything after the slash
			if (Namespace.StartsWith(TEXT("mocap/"), ESearchCase::IgnoreCase))
			{
				Namespace = FString::Printf(TEXT("%s/%s"), Prefix, *Namespace.Mid(6));
			}
			else if (Namespace.StartsWith(TEXT("audio/"), ESearchCase::IgnoreCase))
			{
				Namespace = FString::Printf(TEXT("%s/%s"), Prefix, *Namespace.Mid(6));
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
}
