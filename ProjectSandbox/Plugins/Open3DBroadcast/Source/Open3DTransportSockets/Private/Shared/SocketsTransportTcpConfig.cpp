// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTransportConfig.h"

#include "SocketsTransportCommon.h"
#include "SocketsTcpTransport.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportTypes.h"

// The TCP configure functions read only the config: the sender component and the receiver
// source copy their (non-secret) transport options into Config.AdvancedParams before the
// descriptor's configure function runs. So nothing here includes Open3DSender or Open3DReceiver
// (ADR 0007 step 4, WP-A1 PR 4b). Every tcp.* option the user set is already in the map and
// reaches the transport unchanged.

namespace O3DSocketsTcpConfigPrivate
{
	/** A port option: 1 to 65535 in digits, otherwise Default (TRB-26: "80abc" is no longer port 80). */
	int32 ReadPort(const FO3DTransportConfig& Config, const TCHAR* Key, int32 Default)
	{
		int32 Port = 0;
		const FString Value = O3DTransportOptions::GetString(Config.AdvancedParams, Key);
		return O3DTransportOptions::TryParsePort(Value, Port) ? Port : Default;
	}

	FString MakeTcpUri(const FString& Host, int32 Port)
	{
		FO3DHostPort Endpoint;
		Endpoint.Host = Host;
		Endpoint.Port = Port;
		Endpoint.bIPv6 = Host.Contains(TEXT(":"));
		return FString::Printf(TEXT("tcp://%s"), *Endpoint.ToString());
	}
}

namespace O3DSockets::Tcp
{
	bool ParseTcpEndpoint(const FO3DTransportConfig& Config, FO3DHostPort& OutEndpoint)
	{
		FO3DHostPort Endpoint;
		bool bParsed = false;

		const FString Uri = Config.Uri.TrimStartAndEnd();
		if (Uri.StartsWith(TEXT("tcp://"), ESearchCase::IgnoreCase))
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
}

namespace O3DSocketsConfig
{
	void ConfigureTcpSender(FO3DTransportConfig& Config, const TCHAR* TransportName)
	{
		using namespace O3DSocketsTcpConfigPrivate;
		Config.Transport = TransportName;
		Config.Role = TEXT("sender");

		const FString StoredBindHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::BindOptionKey);
		const FString BindHost = StoredBindHost.IsEmpty() ? FString(TEXT("0.0.0.0")) : O3DSockets::NormaliseHostname(StoredBindHost);
		const int32 Port = ReadPort(Config, O3DSockets::PortOptionKey, DefaultTcpPort);

		Config.Uri = MakeTcpUri(BindHost, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(BindHost, Port);
		Config.AdvancedParams.Add(O3DSockets::BindOptionKey, BindHost);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));

		if (Config.Audio.bEnableAudio)
		{
			const FString StoredAudioBind = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::AudioBindOptionKey);
			const FString AudioBind = StoredAudioBind.IsEmpty() ? BindHost : O3DSockets::NormaliseHostname(StoredAudioBind);
			const int32 AudioPort = ReadPort(Config, O3DSockets::AudioPortOptionKey, Port < 65535 ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioBindOptionKey, AudioBind);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}

	void ConfigureTcpReceiver(FO3DTransportConfig& Config, const TCHAR* TransportName)
	{
		using namespace O3DSocketsTcpConfigPrivate;
		Config.Transport = TransportName;

		const FString StoredHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::HostOptionKey);
		const FString Host = StoredHost.IsEmpty() ? FString(TEXT("127.0.0.1")) : O3DSockets::NormaliseHostname(StoredHost);
		const int32 Port = ReadPort(Config, O3DSockets::PortOptionKey, DefaultTcpPort);

		Config.Uri = MakeTcpUri(Host, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(Host, Port);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, Host);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));

		if (Config.Audio.bEnableAudio)
		{
			const FString StoredAudioHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::AudioHostOptionKey);
			const FString AudioHost = StoredAudioHost.IsEmpty() ? Host : O3DSockets::NormaliseHostname(StoredAudioHost);
			const int32 AudioPort = ReadPort(Config, O3DSockets::AudioPortOptionKey, Port < 65535 ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioHostOptionKey, AudioHost);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
