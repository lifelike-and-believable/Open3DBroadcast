// Copyright 2026 Lifelike & Believable. All Rights Reserved.

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
		const FString BindHost = StoredBindHost.IsEmpty() ? FString(O3DSockets::DefaultListenHost) : O3DSockets::NormaliseHostname(StoredBindHost);
		const int32 Port = O3DSockets::ReadPortOption(Config, O3DSockets::PortOptionKey, DefaultTcpPort);

		Config.Uri = O3DSockets::MakeUri(TEXT("tcp"), BindHost, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(BindHost, Port);
		Config.AdvancedParams.Add(O3DSockets::BindOptionKey, BindHost);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));
	}

	void ConfigureTcpReceiver(FO3DTransportConfig& Config, const TCHAR* TransportName)
	{
		Config.Transport = TransportName;
		Config.Role = EO3DTransportRole::Receiver;

		const FString StoredHost = O3DTransportOptions::GetString(Config.AdvancedParams, O3DSockets::HostOptionKey);
		const FString Host = StoredHost.IsEmpty() ? FString(O3DSockets::DefaultRemoteHost) : O3DSockets::NormaliseHostname(StoredHost);
		const int32 Port = O3DSockets::ReadPortOption(Config, O3DSockets::PortOptionKey, DefaultTcpPort);

		Config.Uri = O3DSockets::MakeUri(TEXT("tcp"), Host, Port);
		Config.StreamId = O3DSockets::ComposeStreamId(Host, Port);
		Config.AdvancedParams.Add(O3DSockets::HostOptionKey, Host);
		Config.AdvancedParams.Add(O3DSockets::PortOptionKey, FString::FromInt(Port));
	}
}

#endif // O3D_WITH_TRANSPORT_SOCKETS
