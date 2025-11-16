#include "SocketsTransportConfig.h"

#include "O3DReceiverSourceSettings.h"
#include "O3DSenderComponent.h"
#include "SocketsTransportCommon.h"

namespace O3DSocketsConfig
{
	void ConfigureTcpSender(const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config, const TCHAR* TransportName)
	{
		Config.Transport = TransportName;
		Config.Role = TEXT("sender");

		const FString StoredBindHost = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::BindOptionKey) : FString();
		const FString BindHost = StoredBindHost.IsEmpty() ? TEXT("0.0.0.0") : O3DSockets::NormaliseHostname(StoredBindHost);

		const FString StoredPort = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::PortOptionKey) : FString();
		const int32 Port = ParsePositiveInt(StoredPort, DefaultTcpPort);

		Config.Uri = O3DSockets::BuildTcpUri(BindHost, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(BindHost, Port);
		Config.AdvancedParams.Add(O3DSockets::BindOptionKey, BindHost);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));

		if (Config.Audio.bEnableAudio)
		{
			const FString StoredAudioBind = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::AudioBindOptionKey) : FString();
			const FString AudioBind = StoredAudioBind.IsEmpty() ? BindHost : O3DSockets::NormaliseHostname(StoredAudioBind);

			const FString StoredAudioPort = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::AudioPortOptionKey) : FString();
			const int32 AudioPort = ParsePositiveInt(StoredAudioPort, (Port > 0) ? (Port + 1) : 0);

			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioBindOptionKey, AudioBind);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}

	void ConfigureTcpReceiver(const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config, const TCHAR* TransportName)
	{
		Config.Transport = TransportName;

		const FString* HostOption = Settings.TransportOptions.Find(O3DSockets::HostOptionKey);
		const FString* PortOption = Settings.TransportOptions.Find(O3DSockets::PortOptionKey);
		const FString Host = HostOption ? O3DSockets::NormaliseHostname(*HostOption) : FString(TEXT("127.0.0.1"));
		const int32 Port = ParsePositiveInt(PortOption ? *PortOption : FString(), DefaultTcpPort);

		Config.Uri = O3DSockets::BuildTcpUri(Host, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(Host, Port);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, Host);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));

		if (Config.Audio.bEnableAudio)
		{
			const FString* AudioHostOption = Settings.TransportOptions.Find(O3DSockets::AudioHostOptionKey);
			const FString AudioHost = AudioHostOption ? O3DSockets::NormaliseHostname(*AudioHostOption) : Host;

			const FString* AudioPortOption = Settings.TransportOptions.Find(O3DSockets::AudioPortOptionKey);
			const int32 AudioPort = ParsePositiveInt(AudioPortOption ? *AudioPortOption : FString(), (Port > 0) ? Port + 1 : 0);
			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioHostOptionKey, AudioHost);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}
}

