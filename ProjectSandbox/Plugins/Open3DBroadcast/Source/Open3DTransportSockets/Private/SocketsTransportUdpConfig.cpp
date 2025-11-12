#include "SocketsTransportConfig.h"

#include "O3DReceiverSourceSettings.h"
#include "O3DSenderComponent.h"
#include "SocketsTransportCommon.h"

namespace O3DSocketsConfig
{
	void ConfigureUdpSender(const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
	{
		Config.Transport = TEXT("sockets.udp");
		Config.Role = TEXT("sender");

		const FString StoredHost = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::HostOptionKey) : FString();
		const FString Host = StoredHost.IsEmpty() ? TEXT("127.0.0.1") : O3DSockets::NormaliseHostname(StoredHost);

		const FString StoredPort = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::PortOptionKey) : FString();
		const int32 Port = ParsePositiveInt(StoredPort, DefaultUdpPort);

		const FString BroadcastValue = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::BroadcastOptionKey) : FString();
		const bool bBroadcast = ParseBoolOption(BroadcastValue, false);

		const FString MtuValue = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::MtuOptionKey) : FString();
		const int32 Mtu = ParsePositiveInt(MtuValue, 1200);

		const FString DatagramValue = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::MaxDatagramOptionKey) : FString();
		const int32 MaxDatagram = ParsePositiveInt(DatagramValue, 64000);

		Config.Uri = O3DSockets::BuildUdpUri(Host, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(Host, Port);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, Host);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));
		Config.AdvancedParams.Add(O3DSockets::BroadcastOptionKey, bBroadcast ? TEXT("true") : TEXT("false"));
		Config.AdvancedParams.Add(O3DSockets::MtuOptionKey, FString::FromInt(Mtu));
		Config.AdvancedParams.Add(O3DSockets::MaxDatagramOptionKey, FString::FromInt(MaxDatagram));

		if (Config.Audio.bEnableAudio)
		{
			const FString StoredAudioHost = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::AudioHostOptionKey) : FString();
			const FString AudioHost = StoredAudioHost.IsEmpty() ? Host : O3DSockets::NormaliseHostname(StoredAudioHost);

			const FString StoredAudioPort = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::AudioPortOptionKey) : FString();
			const int32 AudioPort = ParsePositiveInt(StoredAudioPort, (Port > 0) ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioHostOptionKey, AudioHost);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}

	void ConfigureUdpReceiver(const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
	{
		Config.Transport = TEXT("sockets.udp");

		const FString* HostOption = Settings.TransportOptions.Find(O3DSockets::HostOptionKey);
		const FString Host = HostOption ? O3DSockets::NormaliseHostname(*HostOption) : FString(TEXT("0.0.0.0"));

		const FString* PortOption = Settings.TransportOptions.Find(O3DSockets::PortOptionKey);
		const int32 Port = ParsePositiveInt(PortOption ? *PortOption : FString(), DefaultUdpPort);

		const FString* BroadcastOption = Settings.TransportOptions.Find(O3DSockets::BroadcastOptionKey);
		const bool bBroadcast = ParseBoolOption(BroadcastOption ? *BroadcastOption : FString(), false);

		const FString* MtuOption = Settings.TransportOptions.Find(O3DSockets::MtuOptionKey);
		const int32 Mtu = ParsePositiveInt(MtuOption ? *MtuOption : FString(), 1200);

		const FString* DatagramOption = Settings.TransportOptions.Find(O3DSockets::MaxDatagramOptionKey);
		const int32 MaxDatagram = ParsePositiveInt(DatagramOption ? *DatagramOption : FString(), 64000);

		Config.Uri = O3DSockets::BuildUdpUri(Host, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(Host, Port);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, Host);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));
		Config.AdvancedParams.Add(O3DSockets::BroadcastOptionKey, bBroadcast ? TEXT("true") : TEXT("false"));
		Config.AdvancedParams.Add(O3DSockets::MtuOptionKey, FString::FromInt(Mtu));
		Config.AdvancedParams.Add(O3DSockets::MaxDatagramOptionKey, FString::FromInt(MaxDatagram));

		if (Config.Audio.bEnableAudio)
		{
			const FString* AudioBindOption = Settings.TransportOptions.Find(O3DSockets::AudioBindOptionKey);
			const FString AudioBind = AudioBindOption ? O3DSockets::NormaliseHostname(*AudioBindOption) : Host;

			const FString* AudioPortOption = Settings.TransportOptions.Find(O3DSockets::AudioPortOptionKey);
			const int32 AudioPort = ParsePositiveInt(AudioPortOption ? *AudioPortOption : FString(), (Port > 0) ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioBindOptionKey, AudioBind);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}
}

