#include "Modules/ModuleManager.h"

#include "O3DSenderRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverRegistry.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DReceiverSourceSettings.h"
#include "Sender/SocketsTcpSender.h"
#include "Receiver/SocketsTcpReceiver.h"
#include "Sender/SocketsUdpSender.h"
#include "Receiver/SocketsUdpReceiver.h"
#include "Shared/SocketsTransportCommon.h"
#include "Shared/SocketsTransportConfig.h"

#include "Logging/LogMacros.h"

#if WITH_EDITOR
#include "Shared/SocketsTransportEditorWidgets.h"
#endif // WITH_EDITOR

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportSocketsModule, Log, All);

namespace
{
	constexpr TCHAR SocketsTcpName[] = TEXT("TCP");
	constexpr TCHAR SocketsUdpName[] = TEXT("UDP");
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
		TcpReceiverCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit) -> TSharedPtr<SO3DTransportConfigPanelBase>
		{
			return SocketsEditor::Receiver::BuildTcpReceiverSettingsPanel(SettingsObject, OnSubmit);
		};
#endif // WITH_EDITOR
		O3DReceiver::RegisterTransportCustomization(SocketsTcpName, MoveTemp(TcpReceiverCustomization));

		FO3DReceiverTransportCustomization UdpReceiverCustomization;
		UdpReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureUdpReceiver(Settings, Config);
		};
#if WITH_EDITOR
		UdpReceiverCustomization.BuildTransportWidget = [](UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit) -> TSharedPtr<SO3DTransportConfigPanelBase>
		{
			return SocketsEditor::Receiver::BuildUdpReceiverSettingsPanel(SettingsObject, OnSubmit);
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
