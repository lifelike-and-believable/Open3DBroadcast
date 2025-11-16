#include "SocketsTransportCommon.h"

#include "O3DTransportTypes.h"

namespace
{
	bool ParseHostPortInternal(const FString& Input, FString& OutHost, int32& OutPort)
	{
		FString Working = Input;
		Working.TrimStartAndEndInline();
		if (Working.IsEmpty())
		{
			return false;
		}

		if (Working.StartsWith(TEXT("[")))
		{
			int32 ClosingIndex = 0;
			if (!Working.FindChar(']', ClosingIndex))
			{
				return false;
			}

			FString HostPart = Working.Mid(1, ClosingIndex - 1);
			const int32 ColonIndex = Working.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ClosingIndex);
			if (ColonIndex == INDEX_NONE)
			{
				return false;
			}

			FString PortPart = Working.Mid(ColonIndex + 1);
			PortPart.TrimStartAndEndInline();
			const int32 ParsedPort = FCString::Atoi(*PortPart);
			if (ParsedPort <= 0)
			{
				return false;
			}

			OutHost = HostPart;
			OutPort = ParsedPort;
			return true;
		}

		int32 ColonIndex = INDEX_NONE;
		if (!Working.FindLastChar(':', ColonIndex))
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

		const int32 ParsedPort = FCString::Atoi(*PortPart);
		if (ParsedPort <= 0)
		{
			return false;
		}

		OutHost = HostPart;
		OutPort = ParsedPort;
		return true;
	}
}

namespace O3DSockets
{
	FString NormaliseHostname(const FString& Host)
	{
		FString Result = Host;
		Result.TrimStartAndEndInline();
		return Result.ToLower();
	}

	FString BuildTcpUri(const FString& Host, int32 Port)
	{
		return FString::Printf(TEXT("tcp://%s:%d"), *NormaliseHostname(Host), Port);
	}

	FString BuildUdpUri(const FString& Host, int32 Port)
	{
		return FString::Printf(TEXT("udp://%s:%d"), *NormaliseHostname(Host), Port);
	}

	bool ParseHostPort(const FString& Uri, const TCHAR* Scheme, FString& OutHost, int32& OutPort)
	{
		FString Working = Uri;
		Working.TrimStartAndEndInline();

		if (Scheme && *Scheme)
		{
			const FString Prefix = FString::Printf(TEXT("%s://"), Scheme);
			if (!Working.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				return false;
			}
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

	FString GetOptionValue(const FO3DTransportConfig& Config, const FString& Key)
	{
		for (const TPair<FString, FString>& Pair : Config.AdvancedParams)
		{
			if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
			{
				return Pair.Value;
			}
		}
		return FString();
	}

	int32 GetIntOption(const FO3DTransportConfig& Config, const FString& Key, int32 DefaultValue)
	{
		const FString Value = GetOptionValue(Config, Key);
		if (Value.IsEmpty())
		{
			return DefaultValue;
		}

		return FCString::Atoi(*Value);
	}

	bool GetBoolOption(const FO3DTransportConfig& Config, const FString& Key, bool DefaultValue)
	{
		const FString Value = GetOptionValue(Config, Key);
		if (Value.IsEmpty())
		{
			return DefaultValue;
		}

		if (Value.Equals(TEXT("1"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		if (Value.Equals(TEXT("0"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		return DefaultValue;
	}

	bool ParseHostPort(const FO3DTransportConfig& Config, FString& OutHost, int32& OutPort, const TCHAR* OverrideScheme)
	{
		if (!Config.Uri.IsEmpty())
		{
			if (ParseHostPort(Config.Uri, OverrideScheme, OutHost, OutPort))
			{
				OutHost = NormaliseHostname(OutHost);
				return true;
			}
		}

		FString HostValue = GetOptionValue(Config, HostOptionKey);
		int32 PortValue = GetIntOption(Config, PortOptionKey, 0);
		if (PortValue > 0 && !HostValue.IsEmpty())
		{
			OutHost = NormaliseHostname(HostValue);
			OutPort = PortValue;
			return true;
		}

		if (!Config.StreamId.IsEmpty())
		{
			if (ParseHostPortInternal(Config.StreamId, OutHost, OutPort))
			{
				OutHost = NormaliseHostname(OutHost);
				return true;
			}
		}

		return false;
	}

	FString ComposeStreamId(const FString& Host, int32 Port)
	{
		if (Host.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s:%d"), *Host, Port);
	}
}
