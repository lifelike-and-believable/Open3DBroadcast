#include "Modules/ModuleManager.h"

#include "O3DSenderRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverRegistry.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DReceiverSourceSettings.h"
#include "SocketsTcpSender.h"
#include "SocketsTcpReceiver.h"
#include "SocketsUdpSender.h"
#include "SocketsUdpReceiver.h"
#include "SocketsTransportCommon.h"

#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportSocketsModule, Log, All);

namespace
{
	constexpr TCHAR SocketsLegacyName[] = TEXT("sockets");
	constexpr TCHAR SocketsTcpName[] = TEXT("sockets.tcp");
	constexpr TCHAR SocketsUdpName[] = TEXT("sockets.udp");

	int32 ParsePositiveInt(const FString& Value, int32 DefaultValue)
	{
		if (Value.IsEmpty())
		{
			return DefaultValue;
		}

		const int32 Parsed = FCString::Atoi(*Value);
		return Parsed > 0 ? Parsed : DefaultValue;
	}

	bool ParseBoolOption(const FString& Value, bool DefaultValue)
	{
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

	void ConfigureTcpSender(const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config, const FString& TransportName)
	{
		Config.Transport = TransportName;
		Config.Role = TEXT("sender");

		const FString StoredBindHost = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::BindOptionKey) : FString();
		const FString BindHost = StoredBindHost.IsEmpty() ? TEXT("0.0.0.0") : O3DSockets::NormaliseHostname(StoredBindHost);

		const FString StoredPort = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::PortOptionKey) : FString();
		const int32 Port = ParsePositiveInt(StoredPort, 17700);

		Config.Uri = O3DSockets::BuildTcpUri(BindHost, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(BindHost, Port);
		Config.AdvancedParams.Add(O3DSockets::BindOptionKey, BindHost);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));

		if (Config.Audio.bEnableAudio)
		{
			const FString StoredAudioBind = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::AudioBindOptionKey) : FString();
			const FString AudioBind = StoredAudioBind.IsEmpty() ? BindHost : O3DSockets::NormaliseHostname(StoredAudioBind);

			const FString StoredAudioPort = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::AudioPortOptionKey) : FString();
			const int32 AudioPortDefault = (Port > 0) ? (Port + 1) : 0;
			const int32 AudioPort = ParsePositiveInt(StoredAudioPort, AudioPortDefault);

			if (AudioPort > 0)
			{
				Config.AdvancedParams.Add(O3DSockets::AudioBindOptionKey, AudioBind);
				Config.AdvancedParams.Add(O3DSockets::AudioPortOptionKey, FString::FromInt(AudioPort));
			}
		}
	}

	void ConfigureTcpReceiver(const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config, const FString& TransportName)
	{
		Config.Transport = TransportName;
		const FString* HostOption = Settings.TransportOptions.Find(O3DSockets::HostOptionKey);
		const FString* PortOption = Settings.TransportOptions.Find(O3DSockets::PortOptionKey);
		const FString Host = HostOption ? O3DSockets::NormaliseHostname(*HostOption) : FString(TEXT("127.0.0.1"));
		const int32 Port = ParsePositiveInt(PortOption ? *PortOption : FString(), 17700);

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

	void ConfigureUdpSender(const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
	{
		Config.Transport = TEXT("sockets.udp");
		Config.Role = TEXT("sender");

		const FString StoredHost = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::HostOptionKey) : FString();
		const FString Host = StoredHost.IsEmpty() ? TEXT("127.0.0.1") : O3DSockets::NormaliseHostname(StoredHost);

		const FString StoredPort = SenderComponent ? SenderComponent->GetTransportOption(O3DSockets::PortOptionKey) : FString();
		const int32 Port = ParsePositiveInt(StoredPort, 17800);

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
	}

	void ConfigureUdpReceiver(const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
	{
		Config.Transport = TEXT("sockets.udp");

		const FString* HostOption = Settings.TransportOptions.Find(O3DSockets::HostOptionKey);
		const FString Host = HostOption ? O3DSockets::NormaliseHostname(*HostOption) : FString(TEXT("0.0.0.0"));

		const FString* PortOption = Settings.TransportOptions.Find(O3DSockets::PortOptionKey);
		const int32 Port = ParsePositiveInt(PortOption ? *PortOption : FString(), 17800);

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
	}
}

class FOpen3DTransportSocketsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		O3DTransport::RegisterSender(SocketsLegacyName, []() { return MakeShared<FO3DSocketsTcpSender>(); });
		O3DTransport::RegisterReceiver(SocketsLegacyName, []() { return MakeShared<FO3DSocketsTcpReceiver>(); });

		O3DTransport::RegisterSender(SocketsTcpName, []() { return MakeShared<FO3DSocketsTcpSender>(); });
		O3DTransport::RegisterReceiver(SocketsTcpName, []() { return MakeShared<FO3DSocketsTcpReceiver>(); });

		O3DTransport::RegisterSender(SocketsUdpName, []() { return MakeShared<FO3DSocketsUdpSender>(); });
		O3DTransport::RegisterReceiver(SocketsUdpName, []() { return MakeShared<FO3DSocketsUdpReceiver>(); });

		FO3DSenderTransportCustomization TcpSenderCustomization;
		TcpSenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			ConfigureTcpSender(SenderComponent, Config, TEXT("sockets"));
		};
		O3DSender::RegisterTransportCustomization(SocketsLegacyName, MoveTemp(TcpSenderCustomization));

		FO3DSenderTransportCustomization TcpSenderCustomizationAlias;
		TcpSenderCustomizationAlias.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			ConfigureTcpSender(SenderComponent, Config, TEXT("sockets.tcp"));
		};
		O3DSender::RegisterTransportCustomization(SocketsTcpName, MoveTemp(TcpSenderCustomizationAlias));

		FO3DSenderTransportCustomization UdpSenderCustomization;
		UdpSenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			ConfigureUdpSender(SenderComponent, Config);
		};
		O3DSender::RegisterTransportCustomization(SocketsUdpName, MoveTemp(UdpSenderCustomization));

		FO3DReceiverTransportCustomization TcpReceiverCustomization;
		TcpReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			ConfigureTcpReceiver(Settings, Config, TEXT("sockets"));
		};
		O3DReceiver::RegisterTransportCustomization(SocketsLegacyName, MoveTemp(TcpReceiverCustomization));

		FO3DReceiverTransportCustomization TcpReceiverCustomizationAlias;
		TcpReceiverCustomizationAlias.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			ConfigureTcpReceiver(Settings, Config, TEXT("sockets.tcp"));
		};
		O3DReceiver::RegisterTransportCustomization(SocketsTcpName, MoveTemp(TcpReceiverCustomizationAlias));

		FO3DReceiverTransportCustomization UdpReceiverCustomization;
		UdpReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			ConfigureUdpReceiver(Settings, Config);
		};
		O3DReceiver::RegisterTransportCustomization(SocketsUdpName, MoveTemp(UdpReceiverCustomization));

		UE_LOG(LogOpen3DTransportSocketsModule, Log, TEXT("Open3D sockets transport module started."));
	}

	virtual void ShutdownModule() override
	{
		O3DTransport::UnregisterSender(SocketsLegacyName);
		O3DTransport::UnregisterReceiver(SocketsLegacyName);
		O3DTransport::UnregisterSender(SocketsTcpName);
		O3DTransport::UnregisterReceiver(SocketsTcpName);
		O3DTransport::UnregisterSender(SocketsUdpName);
		O3DTransport::UnregisterReceiver(SocketsUdpName);

		O3DSender::UnregisterTransportCustomization(SocketsLegacyName);
		O3DSender::UnregisterTransportCustomization(SocketsTcpName);
		O3DSender::UnregisterTransportCustomization(SocketsUdpName);

		O3DReceiver::UnregisterTransportCustomization(SocketsLegacyName);
		O3DReceiver::UnregisterTransportCustomization(SocketsTcpName);
		O3DReceiver::UnregisterTransportCustomization(SocketsUdpName);

		UE_LOG(LogOpen3DTransportSocketsModule, Log, TEXT("Open3D sockets transport module shut down."));
	}
};

IMPLEMENT_MODULE(FOpen3DTransportSocketsModule, Open3DTransportSockets)
