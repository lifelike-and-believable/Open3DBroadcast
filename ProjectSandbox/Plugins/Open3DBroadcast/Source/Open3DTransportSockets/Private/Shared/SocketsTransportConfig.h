// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FO3DTransportConfig;

/**
 * Configure functions of the TCP and UDP descriptors. They read only Config: the sender component
 * and the receiver source copy their (non-secret) transport options into Config.AdvancedParams
 * before the configure function runs, so the module needs neither Open3DSender nor Open3DReceiver
 * (ADR 0007 step 4; TCP since WP-A1 PR 4b, UDP since PR 4c).
 */
namespace O3DSocketsConfig
{
	inline constexpr int32 DefaultTcpPort = 17700;
	inline constexpr int32 DefaultUdpPort = 17800;

	void ConfigureTcpSender(FO3DTransportConfig& Config, const TCHAR* TransportName);
	void ConfigureTcpReceiver(FO3DTransportConfig& Config, const TCHAR* TransportName);

	void ConfigureUdpSender(FO3DTransportConfig& Config);
	void ConfigureUdpReceiver(FO3DTransportConfig& Config);
}
