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
#include "SocketsTransportConfig.h"

#include "Logging/LogMacros.h"

#if WITH_EDITOR
#include "SocketsTransportEditorWidgets.h"
#endif // WITH_EDITOR

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportSocketsModule, Log, All);

namespace
{
	constexpr TCHAR SocketsTcpName[] = TEXT("sockets.tcp");
	constexpr TCHAR SocketsUdpName[] = TEXT("sockets.udp");
}

class FOpen3DTransportSocketsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		O3DTransport::RegisterSender(SocketsTcpName, []() { return MakeShared<FO3DSocketsTcpSender>(); });
		O3DTransport::RegisterReceiver(SocketsTcpName, []() { return MakeShared<FO3DSocketsTcpReceiver>(); });

		O3DTransport::RegisterSender(SocketsUdpName, []() { return MakeShared<FO3DSocketsUdpSender>(); });
		O3DTransport::RegisterReceiver(SocketsUdpName, []() { return MakeShared<FO3DSocketsUdpReceiver>(); });

		FO3DSenderTransportCustomization TcpSenderCustomization;
		TcpSenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureTcpSender(SenderComponent, Config, SocketsTcpName);
		};
#if WITH_EDITOR
		TcpSenderCustomization.BuildTransportWidget = [](UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged) -> TSharedPtr<SWidget>
		{
			return SenderComponent ? SocketsEditor::Sender::BuildTcpSenderSettingsPanel(SenderComponent, OnConfigChanged) : nullptr;
		};
#endif // WITH_EDITOR
		O3DSender::RegisterTransportCustomization(SocketsTcpName, MoveTemp(TcpSenderCustomization));

		FO3DSenderTransportCustomization UdpSenderCustomization;
		UdpSenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureUdpSender(SenderComponent, Config);
		};
#if WITH_EDITOR
		UdpSenderCustomization.BuildTransportWidget = [](UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged) -> TSharedPtr<SWidget>
		{
			return SenderComponent ? SocketsEditor::Sender::BuildUdpSenderSettingsPanel(SenderComponent, OnConfigChanged) : nullptr;
		};
#endif // WITH_EDITOR
		O3DSender::RegisterTransportCustomization(SocketsUdpName, MoveTemp(UdpSenderCustomization));

		FO3DReceiverTransportCustomization TcpReceiverCustomization;
		TcpReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureTcpReceiver(Settings, Config, SocketsTcpName);
		};
#if WITH_EDITOR
		TcpReceiverCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject) -> TSharedPtr<SWidget>
		{
			return SettingsObject ? SocketsEditor::Receiver::BuildTcpReceiverSettingsPanel(SettingsObject) : nullptr;
		};
#endif // WITH_EDITOR
		O3DReceiver::RegisterTransportCustomization(SocketsTcpName, MoveTemp(TcpReceiverCustomization));

		FO3DReceiverTransportCustomization UdpReceiverCustomization;
		UdpReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureUdpReceiver(Settings, Config);
		};
#if WITH_EDITOR
		UdpReceiverCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject) -> TSharedPtr<SWidget>
		{
			return SettingsObject ? SocketsEditor::Receiver::BuildUdpReceiverSettingsPanel(SettingsObject) : nullptr;
		};
#endif // WITH_EDITOR
		O3DReceiver::RegisterTransportCustomization(SocketsUdpName, MoveTemp(UdpReceiverCustomization));

		UE_LOG(LogOpen3DTransportSocketsModule, Log, TEXT("Open3D sockets transport module started."));
	}

	virtual void ShutdownModule() override
	{
		O3DTransport::UnregisterSender(SocketsTcpName);
		O3DTransport::UnregisterReceiver(SocketsTcpName);
		O3DTransport::UnregisterSender(SocketsUdpName);
		O3DTransport::UnregisterReceiver(SocketsUdpName);

		O3DSender::UnregisterTransportCustomization(SocketsTcpName);
		O3DSender::UnregisterTransportCustomization(SocketsUdpName);

		O3DReceiver::UnregisterTransportCustomization(SocketsTcpName);
		O3DReceiver::UnregisterTransportCustomization(SocketsUdpName);

		UE_LOG(LogOpen3DTransportSocketsModule, Log, TEXT("Open3D sockets transport module shut down."));
	}
};

IMPLEMENT_MODULE(FOpen3DTransportSocketsModule, Open3DTransportSockets)
