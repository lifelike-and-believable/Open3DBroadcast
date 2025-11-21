// Copyright (c) Open3DStream Contributors

#include "Shared/QuicHelpers.h"

#include "HAL/PlatformMath.h"

namespace O3DQuic
{
	namespace
	{
		FString NormaliseHost(const FString& Host)
		{
			FString Result = Host;
			Result.TrimStartAndEndInline();
			return Result.ToLower();
		}

		bool ParsePort(const FString& Text, uint16& OutPort)
		{
			int32 Parsed = FCString::Atoi(*Text);
			if (Parsed <= 0 || Parsed > 65535)
			{
				return false;
			}
			OutPort = static_cast<uint16>(Parsed);
			return true;
		}

		bool ParseHostPortInternal(const FString& Input, FString& OutHost, uint16& OutPort)
		{
			FString Working = Input;
			Working.TrimStartAndEndInline();
			if (Working.IsEmpty())
			{
				return false;
			}

			if (Working.StartsWith(TEXT("[")))
			{
				int32 ClosingIndex = INDEX_NONE;
				if (!Working.FindChar(TEXT(']'), ClosingIndex))
				{
					return false;
				}

				const FString HostPart = Working.Mid(1, ClosingIndex - 1);
				const int32 ColonIndex = Working.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ClosingIndex);
				if (ColonIndex == INDEX_NONE)
				{
					return false;
				}

				FString PortPart = Working.Mid(ColonIndex + 1);
				PortPart.TrimStartAndEndInline();
				uint16 ParsedPort = 0;
				if (!ParsePort(PortPart, ParsedPort))
				{
					return false;
				}

				OutHost = HostPart;
				OutPort = ParsedPort;
				return true;
			}

			int32 ColonIndex = INDEX_NONE;
			if (!Working.FindLastChar(TEXT(':'), ColonIndex))
			{
				return false;
			}

			FString HostPart = Working.Left(ColonIndex);
			FString PortPart = Working.Mid(ColonIndex + 1);

			HostPart.TrimStartAndEndInline();
			PortPart.TrimStartAndEndInline();

			if (HostPart.IsEmpty() || PortPart.IsEmpty())
			{
				return false;
			}

			uint16 ParsedPort = 0;
			if (!ParsePort(PortPart, ParsedPort))
			{
				return false;
			}

			OutHost = HostPart;
			OutPort = ParsedPort;
			return true;
		}

		bool ParseHostPortUri(const FString& Uri, FString& OutHost, uint16& OutPort)
		{
			FString Working = Uri;
			Working.TrimStartAndEndInline();
			if (Working.IsEmpty())
			{
				return false;
			}

			const FString Prefix = FString::Printf(TEXT("%s://"), QuicSchemeName);
			if (Working.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				Working.RightChopInline(Prefix.Len(), EAllowShrinking::No);
			}
			else
			{
				const int32 SchemeIndex = Working.Find(TEXT("://"));
				if (SchemeIndex != INDEX_NONE)
				{
					Working.RightChopInline(SchemeIndex + 3, EAllowShrinking::No);
				}
			}

			return ParseHostPortInternal(Working, OutHost, OutPort);
		}

		FString ComposeStreamId(const FString& Host, uint16 Port)
		{
			if (Host.IsEmpty() || Port == 0)
			{
				return FString();
			}
			return FString::Printf(TEXT("%s:%u"), *Host, Port);
		}

		const FString* FindAdvancedOption(const FO3DTransportConfig& Config, const FString& Key)
		{
			for (const TPair<FString, FString>& Pair : Config.AdvancedParams)
			{
				if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
				{
					return &Pair.Value;
				}
			}
			return nullptr;
		}

		FString GetAdvancedOption(const FO3DTransportConfig& Config, const TCHAR* Key)
		{
			if (!Key)
			{
				return FString();
			}
			const FString* Value = FindAdvancedOption(Config, Key);
			return Value ? *Value : FString();
		}

		bool ParseBoolOption(const FString& Value, bool DefaultValue)
		{
			if (Value.IsEmpty())
			{
				return DefaultValue;
			}

			if (Value.Equals(TEXT("1"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("yes"), ESearchCase::IgnoreCase))
			{
				return true;
			}

			if (Value.Equals(TEXT("0"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) ||
				Value.Equals(TEXT("no"), ESearchCase::IgnoreCase))
			{
				return false;
			}

			return DefaultValue;
		}

		uint8 ParsePriority(const FString& Text, uint8 DefaultValue)
		{
			if (Text.IsEmpty())
			{
				return DefaultValue;
			}

			int32 Parsed = FCString::Atoi(*Text);
			Parsed = FMath::Clamp(Parsed, 0, 255);
			return static_cast<uint8>(Parsed);
		}

		O3DMoQ::EMoQReliabilityMode ParseReliability(const FString& Text, O3DMoQ::EMoQReliabilityMode DefaultMode)
		{
			if (Text.IsEmpty())
			{
				return DefaultMode;
			}

			if (Text.Equals(TEXT("reliable"), ESearchCase::IgnoreCase) ||
				Text.Equals(TEXT("reliable_ordered"), ESearchCase::IgnoreCase))
			{
				return O3DMoQ::EMoQReliabilityMode::ReliableOrdered;
			}

			if (Text.Equals(TEXT("unreliable"), ESearchCase::IgnoreCase) ||
				Text.Equals(TEXT("unreliable_sequenced"), ESearchCase::IgnoreCase))
			{
				return O3DMoQ::EMoQReliabilityMode::UnreliableSequenced;
			}

			return DefaultMode;
		}

		FString DetermineTrackName(const FO3DTransportConfig& Config)
		{
			FString TrackName = GetAdvancedOption(Config, SenderTrackNameOptionKey);
			if (!TrackName.IsEmpty())
			{
				return TrackName;
			}

			if (!Config.StreamId.IsEmpty())
			{
				return Config.StreamId;
			}

			return DefaultTrackName;
		}

		bool ExtractEndpoint(const FO3DTransportConfig& Config, FString& OutHost, uint16& OutPort)
		{
			FString Host;
			uint16 Port = 0;

			if (!Config.Uri.IsEmpty())
			{
				if (ParseHostPortUri(Config.Uri, Host, Port))
				{
					OutHost = NormaliseHost(Host);
					OutPort = Port;
					return true;
				}
			}

			FString HostOption = GetAdvancedOption(Config, SenderHostOptionKey);
			FString PortOption = GetAdvancedOption(Config, SenderPortOptionKey);
			uint16 PortCandidate = 0;
			if (!HostOption.IsEmpty() && !PortOption.IsEmpty() && ParsePort(PortOption, PortCandidate))
			{
				OutHost = NormaliseHost(HostOption);
				OutPort = PortCandidate;
				return true;
			}

			if (!Config.StreamId.IsEmpty())
			{
				if (ParseHostPortInternal(Config.StreamId, Host, Port))
				{
					OutHost = NormaliseHost(Host);
					OutPort = Port;
					return true;
				}
			}

			return false;
		}
	}

	FString FQuicSenderOptions::Describe() const
	{
		return FString::Printf(TEXT("[endpoint=%s:%u track=%s priority=%d reliability=%s datagrams=%d]"),
			Endpoint.Host.IsEmpty() ? TEXT("<unset>") : *Endpoint.Host,
			Endpoint.Port,
			TrackName.IsEmpty() ? TEXT("<unset>") : *TrackName,
			TrackProperties.Priority,
			*O3DMoQ::ReliabilityToString(TrackProperties.Reliability),
			bEnableDatagrams ? 1 : 0);
	}

	bool ParseSenderOptions(const FO3DTransportConfig& Config, FQuicSenderOptions& OutOptions, FString& OutError)
	{
		OutError.Reset();

		FString Host;
		uint16 Port = 0;
		if (!ExtractEndpoint(Config, Host, Port))
		{
			// Fall back to defaults if no endpoint specified.
			Host = TEXT("0.0.0.0");
			Port = DefaultQuicPort;
		}

		if (Port == 0)
		{
			Port = DefaultQuicPort;
		}

		Host = NormaliseHost(Host);
		OutOptions.Endpoint.Host = Host;
		OutOptions.Endpoint.Port = Port;
		OutOptions.StreamId = ComposeStreamId(Host, Port);

		OutOptions.TrackName = DetermineTrackName(Config);
		if (!O3DMoQ::ValidateTrackName(OutOptions.TrackName, OutError))
		{
			return false;
		}

		OutOptions.TrackProperties = O3DMoQ::FMoQTrackProperties();
		OutOptions.TrackProperties.Priority = ParsePriority(GetAdvancedOption(Config, SenderPriorityOptionKey), OutOptions.TrackProperties.Priority);
		OutOptions.TrackProperties.Reliability = ParseReliability(GetAdvancedOption(Config, SenderReliabilityOptionKey), OutOptions.TrackProperties.Reliability);
		OutOptions.TrackProperties.bDatagramsAllowed = ParseBoolOption(GetAdvancedOption(Config, SenderDatagramOptionKey), true);
		OutOptions.TrackProperties.ClampPriority();

		OutOptions.bEnableDatagrams = OutOptions.TrackProperties.bDatagramsAllowed;
		OutOptions.Alpn = DefaultAlpn;

		return true;
	}
}
