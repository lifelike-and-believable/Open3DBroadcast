// Copyright Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_SOCKETS

#include "O3DSenderRegistry.h"
#include "O3DSenderTransportCustomization.h"
#include "O3DSenderComponent.h"
#include "O3DReceiverRegistry.h"
#include "O3DReceiverTransportCustomization.h"
#include "O3DReceiverSourceSettings.h"
#include "O3DTransportOptionSchema.h"
#include "Sender/SocketsTcpSender.h"
#include "Receiver/SocketsTcpReceiver.h"
#include "Sender/SocketsUdpSender.h"
#include "Receiver/SocketsUdpReceiver.h"
#include "Shared/SocketsTransportCommon.h"
#include "Shared/SocketsTransportConfig.h"

#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportSocketsModule, Log, All);

#define LOCTEXT_NAMESPACE "Open3DTransportSockets"

namespace
{
	constexpr TCHAR SocketsTcpName[] = TEXT("TCP");
	constexpr TCHAR SocketsUdpName[] = TEXT("UDP");
}

/**
 * Option schemas (ADR 0010 §4). The editor module renders them; the defaults are the ones
 * O3DSocketsConfig::Configure* apply when a key is unset, shown as hints and never written.
 */
namespace SocketsSchema
{
	static FO3DTransportOptionField MakeText(const TCHAR* InKey, FText InDisplayName, FText InTooltip, const TCHAR* InDefault)
	{
		FO3DTransportOptionField Field;
		Field.Key = InKey;
		Field.DisplayName = MoveTemp(InDisplayName);
		Field.Tooltip = MoveTemp(InTooltip);
		Field.Type = EO3DTransportOptionType::String;
		Field.Default = InDefault;
		return Field;
	}

	static FO3DTransportOptionField MakeInt(const TCHAR* InKey, FText InDisplayName, FText InTooltip, int32 InDefault, int32 InMin, int32 InMax)
	{
		FO3DTransportOptionField Field;
		Field.Key = InKey;
		Field.DisplayName = MoveTemp(InDisplayName);
		Field.Tooltip = MoveTemp(InTooltip);
		Field.Type = EO3DTransportOptionType::Int;
		Field.Default = FString::FromInt(InDefault);
		Field.Min = InMin;
		Field.Max = InMax;
		return Field;
	}

	static FO3DTransportOptionField MakeBool(const TCHAR* InKey, FText InDisplayName, FText InTooltip)
	{
		FO3DTransportOptionField Field;
		Field.Key = InKey;
		Field.DisplayName = MoveTemp(InDisplayName);
		Field.Tooltip = MoveTemp(InTooltip);
		Field.Type = EO3DTransportOptionType::Bool;
		Field.Default = TEXT("false");
		return Field;
	}

	static FO3DTransportOptionField MakePort(int32 InDefault)
	{
		return MakeInt(O3DSockets::PortOptionKey, LOCTEXT("PortLabel", "Port"),
			LOCTEXT("PortTooltip", "TCP or UDP port. The audio stream uses the next port unless audio.port is set."), InDefault, 1, 65535);
	}

	static void AddUdpSizeFields(FO3DTransportOptionSchema& Schema)
	{
		Schema.Add(MakeInt(O3DSockets::MtuOptionKey, LOCTEXT("UdpMtuLabel", "MTU"),
			LOCTEXT("UdpMtuTooltip", "Largest datagram payload sent before a frame is split into fragments."), 1200, 256, 65507));
		Schema.Add(MakeInt(O3DSockets::MaxDatagramOptionKey, LOCTEXT("UdpMaxDatagramLabel", "Max Datagram Bytes"),
			LOCTEXT("UdpMaxDatagramTooltip", "Largest datagram accepted. Keep it at least as large as the MTU."), 64000, 512, 65507));
	}

	static FO3DTransportOptionSchema MakeTcpSender()
	{
		FO3DTransportOptionSchema Schema;
		Schema.Add(MakeText(O3DSockets::BindOptionKey, LOCTEXT("TcpSenderBindLabel", "Bind Address"),
			LOCTEXT("TcpSenderBindTooltip", "Local address the sender listens on for receivers. 0.0.0.0 listens on every interface."), TEXT("0.0.0.0")));
		Schema.Add(MakePort(O3DSocketsConfig::DefaultTcpPort));
		return Schema;
	}

	static FO3DTransportOptionSchema MakeUdpSender()
	{
		FO3DTransportOptionSchema Schema;
		Schema.Add(MakeText(O3DSockets::HostOptionKey, LOCTEXT("UdpSenderHostLabel", "Destination Host"),
			LOCTEXT("UdpSenderHostTooltip", "Address the datagrams are sent to."), TEXT("127.0.0.1")));
		Schema.Add(MakePort(O3DSocketsConfig::DefaultUdpPort));
		Schema.Add(MakeBool(O3DSockets::BroadcastOptionKey, LOCTEXT("UdpSenderBroadcastLabel", "Enable UDP Broadcast"),
			LOCTEXT("UdpSenderBroadcastTooltip", "Allow sending to a broadcast address.")));
		AddUdpSizeFields(Schema);
		return Schema;
	}

	static FO3DTransportOptionSchema MakeTcpReceiver()
	{
		FO3DTransportOptionSchema Schema;
		Schema.Add(MakeText(O3DSockets::HostOptionKey, LOCTEXT("TcpReceiverHostLabel", "Remote Host"),
			LOCTEXT("TcpReceiverHostTooltip", "Address of the TCP sender to connect to."), TEXT("127.0.0.1")));
		Schema.Add(MakePort(O3DSocketsConfig::DefaultTcpPort));
		Schema.Add(MakeInt(O3DSockets::TimeoutOptionKey, LOCTEXT("TcpReceiverTimeoutLabel", "Connection Timeout (seconds)"),
			LOCTEXT("TcpReceiverTimeoutTooltip", "Reconnect if no data received for this many seconds (1-60)"), 5, 1, 60));
		return Schema;
	}

	static FO3DTransportOptionSchema MakeUdpReceiver()
	{
		FO3DTransportOptionSchema Schema;
		Schema.Add(MakeText(O3DSockets::HostOptionKey, LOCTEXT("UdpReceiverBindLabel", "Bind Address"),
			LOCTEXT("UdpReceiverBindTooltip", "Local address the receiver listens on. 0.0.0.0 listens on every interface."), TEXT("0.0.0.0")));
		Schema.Add(MakePort(O3DSocketsConfig::DefaultUdpPort));
		Schema.Add(MakeBool(O3DSockets::BroadcastOptionKey, LOCTEXT("UdpReceiverBroadcastLabel", "Accept Broadcast Packets"),
			LOCTEXT("UdpReceiverBroadcastTooltip", "Receive datagrams sent to a broadcast address.")));
		AddUdpSizeFields(Schema);
		return Schema;
	}
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
		TcpSenderCustomization.OptionSchema = SocketsSchema::MakeTcpSender();
		O3DSender::RegisterTransportCustomization(SocketsTcpName, MoveTemp(TcpSenderCustomization));

		FO3DSenderTransportCustomization UdpSenderCustomization;
		UdpSenderCustomization.ConfigureTransport = [](const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureUdpSender(SenderComponent, Config);
		};
		UdpSenderCustomization.OptionSchema = SocketsSchema::MakeUdpSender();
		O3DSender::RegisterTransportCustomization(SocketsUdpName, MoveTemp(UdpSenderCustomization));

		FO3DReceiverTransportCustomization TcpReceiverCustomization;
		TcpReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureTcpReceiver(Settings, Config, SocketsTcpName);
		};
		TcpReceiverCustomization.OptionSchema = SocketsSchema::MakeTcpReceiver();
		O3DReceiver::RegisterTransportCustomization(SocketsTcpName, MoveTemp(TcpReceiverCustomization));

		FO3DReceiverTransportCustomization UdpReceiverCustomization;
		UdpReceiverCustomization.ConfigureTransport = [](const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureUdpReceiver(Settings, Config);
		};
		UdpReceiverCustomization.OptionSchema = SocketsSchema::MakeUdpReceiver();
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

#undef LOCTEXT_NAMESPACE

#else // O3D_WITH_TRANSPORT_SOCKETS

DEFINE_LOG_CATEGORY_STATIC(LogOpen3DTransportSocketsModule, Log, All);

/**
 * Stub module, compiled when O3D_WITH_TRANSPORT_SOCKETS is 0: the transport was switched off with
 * that environment variable
 * (O3DBuildFlags in Open3DBroadcastBuildFlags.Build.cs). It registers nothing.
 */
class FOpen3DTransportSocketsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UE_LOG(LogOpen3DTransportSocketsModule, Display, TEXT("Open3D sockets transport is not available in this build (O3D_WITH_TRANSPORT_SOCKETS=0)."));
	}

	virtual void ShutdownModule() override {}
};

#endif // O3D_WITH_TRANSPORT_SOCKETS

IMPLEMENT_MODULE(FOpen3DTransportSocketsModule, Open3DTransportSockets)
