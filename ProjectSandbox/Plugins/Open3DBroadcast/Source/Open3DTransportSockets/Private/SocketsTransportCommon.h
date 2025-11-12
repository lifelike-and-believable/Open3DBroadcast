#pragma once

#include "CoreMinimal.h"

struct FO3DTransportConfig;

namespace O3DSockets
{
	static constexpr TCHAR HostOptionKey[] = TEXT("host");
	static constexpr TCHAR PortOptionKey[] = TEXT("port");
	static constexpr TCHAR BindOptionKey[] = TEXT("bind");
	static constexpr TCHAR AudioPortOptionKey[] = TEXT("audio.port");
	static constexpr TCHAR AudioBindOptionKey[] = TEXT("audio.bind");
	static constexpr TCHAR AudioHostOptionKey[] = TEXT("audio.host");
	static constexpr TCHAR BroadcastOptionKey[] = TEXT("udp.broadcast");
	static constexpr TCHAR MtuOptionKey[] = TEXT("udp.mtu");
	static constexpr TCHAR MaxDatagramOptionKey[] = TEXT("udp.maxdatagram");

	/** Trim and normalise a hostname for logging / URI generation. */
	FString NormaliseHostname(const FString& Host);

	/** Compose a canonical tcp://host:port URI. */
	FString BuildTcpUri(const FString& Host, int32 Port);

	/** Compose a canonical udp://host:port URI. */
	FString BuildUdpUri(const FString& Host, int32 Port);

	/**
	 * Parse host/port from a transport config. If OverrideScheme is provided the URI must match it; otherwise any scheme is accepted.
	 */
	bool ParseHostPort(const FO3DTransportConfig& Config, FString& OutHost, int32& OutPort, const TCHAR* OverrideScheme = nullptr);

	/** Parse host/port from a URI with the expected scheme (e.g., tcp or udp). */
	bool ParseHostPort(const FString& Uri, const TCHAR* Scheme, FString& OutHost, int32& OutPort);

	/** Retrieve a case-insensitive advanced option value. */
	FString GetOptionValue(const FO3DTransportConfig& Config, const FString& Key);

	int32 GetIntOption(const FO3DTransportConfig& Config, const FString& Key, int32 DefaultValue);

	bool GetBoolOption(const FO3DTransportConfig& Config, const FString& Key, bool DefaultValue);

	/** Utility for generating a human readable stream id (host:port). */
	FString ComposeStreamId(const FString& Host, int32 Port);
}
