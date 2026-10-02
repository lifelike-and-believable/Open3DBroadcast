// Copyright Lifelike & Believable. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "Logging/LogMacros.h"

#if O3D_WITH_TRANSPORT_SOCKETS

#include "O3DTransportOptionSchema.h"
#include "Transport/O3DTransportRegistry.h"
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
		// One descriptor per transport name (ADR 0007 item 4, WP-A1).
		FO3DTransportDescriptor Tcp;
		Tcp.Name = SocketsTcpName;
		Tcp.OwningModule = TEXT("Open3DTransportSockets");
		Tcp.CreateSender = []() { return MakeShared<FO3DSocketsTcpSender>(); };
		Tcp.CreateReceiver = []() { return MakeShared<FO3DSocketsTcpReceiver>(); };
		Tcp.GetCapabilities = [](const FO3DTransportConfig& Config) { return O3DSockets::GetTcpCapabilities(Config); };
		Tcp.ConfigureSender = [](const UO3DSenderComponent* /*SenderComponent*/, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureTcpSender(Config, SocketsTcpName);
		};
		Tcp.ConfigureReceiver = [](const FO3DReceiverSourceConfig& /*Settings*/, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureTcpReceiver(Config, SocketsTcpName);
		};
		Tcp.SenderOptions.OptionSchema = SocketsSchema::MakeTcpSender();
		Tcp.ReceiverOptions.OptionSchema = SocketsSchema::MakeTcpReceiver();
		TcpRegistration = FO3DTransportRegistry::Get().Register(MoveTemp(Tcp));

		FO3DTransportDescriptor Udp;
		Udp.Name = SocketsUdpName;
		Udp.OwningModule = TEXT("Open3DTransportSockets");
		Udp.CreateSender = []() { return MakeShared<FO3DSocketsUdpSender>(); };
		Udp.CreateReceiver = []() { return MakeShared<FO3DSocketsUdpReceiver>(); };
		Udp.GetCapabilities = [](const FO3DTransportConfig& Config) { return O3DSockets::GetUdpCapabilities(Config); };
		Udp.ConfigureSender = [](const UO3DSenderComponent* /*SenderComponent*/, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureUdpSender(Config);
		};
		Udp.ConfigureReceiver = [](const FO3DReceiverSourceConfig& /*Settings*/, FO3DTransportConfig& Config)
		{
			O3DSocketsConfig::ConfigureUdpReceiver(Config);
		};
		Udp.SenderOptions.OptionSchema = SocketsSchema::MakeUdpSender();
		Udp.ReceiverOptions.OptionSchema = SocketsSchema::MakeUdpReceiver();
		UdpRegistration = FO3DTransportRegistry::Get().Register(MoveTemp(Udp));

		UE_LOG(LogOpen3DTransportSocketsModule, Log, TEXT("Open3D sockets transport module started."));
	}

	virtual void ShutdownModule() override
	{
		// Unregistering drains the transport (ADR 0007 item 5, WP-A1 PR 2): sender components and
		// LiveLink sources stop and release their instances, and the registry stops and reports any
		// left, before this module's code goes away. There is no FFI handle to free afterwards.
		TcpRegistration.Reset();
		UdpRegistration.Reset();

		UE_LOG(LogOpen3DTransportSocketsModule, Log, TEXT("Open3D sockets transport module shut down."));
	}

private:
	FO3DTransportRegistration TcpRegistration;
	FO3DTransportRegistration UdpRegistration;
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
