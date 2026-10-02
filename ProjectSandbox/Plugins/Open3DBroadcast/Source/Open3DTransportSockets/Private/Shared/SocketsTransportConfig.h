// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UO3DSenderComponent;
struct FO3DReceiverSourceConfig;
struct FO3DTransportConfig;

namespace O3DSocketsConfig
{
	inline constexpr int32 DefaultTcpPort = 17700;
	inline constexpr int32 DefaultUdpPort = 17800;

	int32 ParsePositiveInt(const FString& Value, int32 DefaultValue);
	bool ParseBoolOption(const FString& Value, bool DefaultValue);

	/**
	 * TCP: read only Config (the component's and source's options are already in
	 * Config.AdvancedParams), so TCP needs neither Open3DSender nor Open3DReceiver (WP-A1 PR 4b).
	 */
	void ConfigureTcpSender(FO3DTransportConfig& Config, const TCHAR* TransportName);
	void ConfigureTcpReceiver(FO3DTransportConfig& Config, const TCHAR* TransportName);

	/** UDP: still read the component and the source settings until UDP migrates (WP-A1 PR 4c). */

	void ConfigureUdpSender(const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config);
	void ConfigureUdpReceiver(const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config);
}
