// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTransportConfig.h"

#include "SocketsTransportCommon.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportTypes.h"

// The TCP configure functions read only the config: the sender component and the receiver
// source copy their (non-secret) transport options into Config.AdvancedParams before the
// descriptor's configure function runs. So nothing here includes Open3DSender or Open3DReceiver
// (ADR 0007 step 4, WP-A1 PR 4b). Every tcp.* option the user set is already in the map and
// reaches the transport unchanged.

namespace O3DSocketsConfig
{
	void ConfigureTcpSender(FO3DTransportConfig& Config, const TCHAR* TransportName)
	{
		Config.Transport = TransportName;
		Config.Role = EO3DTransportRole::Sender;

		const FString StoredBindHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::BindOptionKey);
		const FString BindHost = StoredBindHost.IsEmpty() ? FString(TEXT("0.0.0.0")) : O3DSockets::NormaliseHostname(StoredBindHost);
		const int32 Port = O3DSockets::ReadPortOption(Config, O3DSockets::PortOptionKey, DefaultTcpPort);

		Config.Uri = O3DSockets::MakeUri(TEXT("tcp"), BindHost, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(BindHost, Port);
		Config.AdvancedParams.Add(O3DSockets::BindOptionKey, BindHost);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));

		if (Config.Audio.bEnableAudio)
		{
			const FString StoredAudioBind = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::AudioBindOptionKey);
			const FString AudioBind = StoredAudioBind.IsEmpty() ? BindHost : O3DSockets::NormaliseHostname(StoredAudioBind);
			const int32 AudioPort = O3DSockets::ReadPortOption(Config, O3DSockets::AudioPortOptionKey, Port < 65535 ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioBindOptionKey, AudioBind);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}

	void ConfigureTcpReceiver(FO3DTransportConfig& Config, const TCHAR* TransportName)
	{
		Config.Transport = TransportName;
		Config.Role = EO3DTransportRole::Receiver;

		const FString StoredHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::HostOptionKey);
		const FString Host = StoredHost.IsEmpty() ? FString(TEXT("127.0.0.1")) : O3DSockets::NormaliseHostname(StoredHost);
		const int32 Port = O3DSockets::ReadPortOption(Config, O3DSockets::PortOptionKey, DefaultTcpPort);

		Config.Uri = O3DSockets::MakeUri(TEXT("tcp"), Host, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(Host, Port);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, Host);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));

		if (Config.Audio.bEnableAudio)
		{
			const FString StoredAudioHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::AudioHostOptionKey);
			const FString AudioHost = StoredAudioHost.IsEmpty() ? Host : O3DSockets::NormaliseHostname(StoredAudioHost);
			const int32 AudioPort = O3DSockets::ReadPortOption(Config, O3DSockets::AudioPortOptionKey, Port < 65535 ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioHostOptionKey, AudioHost);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
