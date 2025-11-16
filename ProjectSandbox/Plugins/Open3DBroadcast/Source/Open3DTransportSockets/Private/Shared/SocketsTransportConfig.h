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

	void ConfigureTcpSender(const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config, const TCHAR* TransportName);
	void ConfigureTcpReceiver(const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config, const TCHAR* TransportName);

	void ConfigureUdpSender(const UO3DSenderComponent* SenderComponent, FO3DTransportConfig& Config);
	void ConfigureUdpReceiver(const FO3DReceiverSourceConfig& Settings, FO3DTransportConfig& Config);
}
