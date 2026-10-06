// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_SOCKETS // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "SocketsTransportConfig.h"

#include "SocketsTransportCommon.h"
#include "Transport/O3DTransportOptions.h"
#include "Transport/O3DTransportTypes.h"

// The UDP configure functions read only the config, as the TCP ones do since WP-A1 PR 4b: the
// sender component and the receiver source copy their (non-secret) options into
// Config.AdvancedParams before the configure function runs, so nothing in Open3DTransportSockets
// includes Open3DSender or Open3DReceiver (ADR 0007 step 4, WP-A1 PR 4c).

namespace O3DSocketsUdpConfigPrivate
{
	/** A positive integer option; Default when absent, not a number or not positive. */
	int32 ReadPositiveInt(const FO3DTransportConfig& Config, const TCHAR* Key, int32 Default)
	{
		const int32 Value = O3DTransportOptions::GetInt(Config.AdvancedParams, Key, Default);
		return Value > 0 ? Value : Default;
	}

	/** Host, port, broadcast and the largest datagram, normalised and written back (both roles). */
	void ConfigureUdp(FO3DTransportConfig& Config, const TCHAR* DefaultHost)
	{
		Config.Transport = TEXT("UDP");

		const FString StoredHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::HostOptionKey);
		const FString Host = StoredHost.IsEmpty() ? FString(DefaultHost) : O3DSockets::NormaliseHostname(StoredHost);
		const int32 Port = O3DSockets::ReadPortOption(Config, O3DSockets::PortOptionKey, O3DSocketsConfig::DefaultUdpPort);
		const bool bBroadcast = O3DTransportOptions::GetBool(Config.AdvancedParams, O3DSockets::BroadcastOptionKey, false);
		const int32 MaxDatagram = ReadPositiveInt(Config, O3DSockets::MaxDatagramOptionKey, 64000);

		Config.Uri = O3DSockets::MakeUri(TEXT("udp"), Host, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(Host, Port);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, Host);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));
		Config.AdvancedParams.Add(O3DSockets::BroadcastOptionKey, bBroadcast ? TEXT("true") : TEXT("false"));
		Config.AdvancedParams.Add(O3DSockets::MaxDatagramOptionKey, FString::FromInt(MaxDatagram));
	}
}

namespace O3DSocketsConfig
{
	void ConfigureUdpSender(FO3DTransportConfig& Config)
	{
		O3DSocketsUdpConfigPrivate::ConfigureUdp(Config, TEXT("127.0.0.1"));
		Config.Role = EO3DTransportRole::Sender;
		// Only the sender fragments frames, so only it has an MTU (TRB-21).
		const int32 Mtu = O3DSocketsUdpConfigPrivate::ReadPositiveInt(Config, O3DSockets::MtuOptionKey, 1200);
		Config.AdvancedParams.Add(O3DSockets::MtuOptionKey, FString::FromInt(Mtu));

		if (Config.Audio.bEnableAudio)
		{
			const FString Host = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::HostOptionKey);
			const int32 Port = O3DSockets::ReadPortOption(Config, O3DSockets::PortOptionKey, DefaultUdpPort);
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

	void ConfigureUdpReceiver(FO3DTransportConfig& Config)
	{
		O3DSocketsUdpConfigPrivate::ConfigureUdp(Config, TEXT("0.0.0.0"));
		Config.Role = EO3DTransportRole::Receiver;

		if (Config.Audio.bEnableAudio)
		{
			const FString Host = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::HostOptionKey);
			const int32 Port = O3DSockets::ReadPortOption(Config, O3DSockets::PortOptionKey, DefaultUdpPort);
			const FString StoredAudioBind = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::AudioBindOptionKey);
			const FString AudioBind = StoredAudioBind.IsEmpty() ? Host : O3DSockets::NormaliseHostname(StoredAudioBind);
			const int32 AudioPort = O3DSockets::ReadPortOption(Config, O3DSockets::AudioPortOptionKey, Port < 65535 ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioBindOptionKey, AudioBind);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
