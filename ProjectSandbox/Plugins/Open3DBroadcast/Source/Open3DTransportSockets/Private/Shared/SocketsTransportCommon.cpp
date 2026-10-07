// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTransportCommon.h"
#include "SocketsTcpTransport.h"

#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportTypes.h"

namespace O3DSockets
{
	bool IsIPv4Multicast(const FString& Host)
	{
		TArray<FString> Octets;
		Host.TrimStartAndEnd().ParseIntoArray(Octets, TEXT("."), /*CullEmpty=*/false);
		if (Octets.Num() != 4)
		{
			return false;
		}
		int32 Values[4] = {};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			int64 Value = 0;
			if (Octets[Index].IsEmpty() || Octets[Index].Len() > 3 || !O3DTransportOptions::TryParseInt(Octets[Index], Value) || Value < 0 || Value > 255)
			{
				return false;
			}
			Values[Index] = static_cast<int32>(Value);
		}
		return Values[0] >= 224 && Values[0] <= 239;
	}

	FString NormaliseHostname(const FString& Host)
	{
		FString Result = Host;
		Result.TrimStartAndEndInline();
		return Result.ToLower();
	}

	FString ComposeStreamId(const FString& Host, int32 Port)
	{
		if (Host.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s:%d"), *Host, Port);
	}

	FString MakeUri(const TCHAR* Scheme, const FString& Host, int32 Port)
	{
		FO3DHostPort Endpoint;
		Endpoint.Host = Host;
		Endpoint.Port = Port;
		Endpoint.bIPv6 = Host.Contains(TEXT(":"));
		return FString::Printf(TEXT("%s://%s"), Scheme, *Endpoint.ToString());
	}

	int32 ReadPortOption(const FO3DTransportConfig& Config, const TCHAR* Key, int32 Default)
	{
		int32 Port = 0;
		const FString Value = O3DTransportOptions::GetString(Config.AdvancedParams, Key);
		return O3DTransportOptions::TryParsePort(Value, Port) ? Port : Default;
	}

	bool ParseEndpoint(const FO3DTransportConfig& Config, const TCHAR* Scheme, FO3DHostPort& OutEndpoint)
	{
		FO3DHostPort Endpoint;
		bool bParsed = false;

		const FString Uri = Config.Uri.TrimStartAndEnd();
		const FString Prefix = FString::Printf(TEXT("%s://"), Scheme);
		if (Uri.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			bParsed = O3DTransportOptions::ParseHostPort(Uri, Endpoint);
		}
		if (!bParsed)
		{
			const FString Host = O3DTransportOptions::GetString(Config.AdvancedParams, HostOptionKey);
			int32 Port = 0;
			if (!Host.IsEmpty() && O3DTransportOptions::TryParsePort(O3DTransportOptions::GetString(Config.AdvancedParams, PortOptionKey), Port))
			{
				bParsed = O3DTransportOptions::ParseHostPort(Host, Endpoint, Port);
			}
		}
		if (!bParsed && !Config.StreamId.IsEmpty())
		{
			bParsed = O3DTransportOptions::ParseHostPort(Config.StreamId, Endpoint);
		}
		if (!bParsed)
		{
			return false;
		}

		Endpoint.Host = NormaliseHostname(Endpoint.Host);
		OutEndpoint = MoveTemp(Endpoint);
		return true;
	}

	FO3DTransportCapabilities GetTcpCapabilities(const FO3DTransportConfig& /*Config*/)
	{
		FO3DTransportCapabilities Caps;
		Caps.bSend = true;
		Caps.bReceive = true;
		Caps.bAudioSend = true;
		Caps.bAudioReceive = true;
		Caps.bControl = true;
		Caps.bBidirectional = true; // one stream socket per receiver; nothing reads the back direction in v1
		Caps.bPeerJoinSignal = true; // the sender reports each accepted receiver (ADR 0005 (vi))
		Caps.Delivery = EO3DDeliveryGuarantee::ReliableOrdered; // ADR 0005 (iii)
		Caps.MaxPayloadBytes = O3DSockets::Tcp::MaxFrameBytesLimit; // the frame header's hard limit
		return Caps;
	}

	FO3DTransportCapabilities GetUdpCapabilities(const FO3DTransportConfig& /*Config*/)
	{
		FO3DTransportCapabilities Caps;
		Caps.bSend = true;
		Caps.bReceive = true;
		Caps.bAudioSend = true;
		Caps.bAudioReceive = true;
		Caps.bControl = true;
		Caps.Delivery = EO3DDeliveryGuarantee::Unreliable; // ADR 0005 (iii)
		// Frames above udp.maxdatagram are fragmented; the limit that matters is the receiver's
		// udp.maxframe, which the sender does not know, so the sender sets none of its own.
		Caps.MaxPayloadBytes = 0;
		return Caps;
	}
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
