// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DTransportOptions.h"

struct FO3DTransportConfig;
struct FO3DTransportCapabilities;

namespace O3DSockets
{
	static constexpr TCHAR HostOptionKey[] = TEXT("host");
	static constexpr TCHAR PortOptionKey[] = TEXT("port");
	static constexpr TCHAR BindOptionKey[] = TEXT("bind");
	static constexpr TCHAR AudioPortOptionKey[] = TEXT("audio.port");
	static constexpr TCHAR AudioBindOptionKey[] = TEXT("audio.bind");
	static constexpr TCHAR AudioHostOptionKey[] = TEXT("audio.host");
	static constexpr TCHAR TimeoutOptionKey[] = TEXT("tcp.timeout");
	static constexpr TCHAR BroadcastOptionKey[] = TEXT("udp.broadcast");
	static constexpr TCHAR MtuOptionKey[] = TEXT("udp.mtu");
	static constexpr TCHAR MaxDatagramOptionKey[] = TEXT("udp.maxdatagram");
	/** Smallest UDP MTU: the 24-byte fragment header plus 256 bytes of payload (WP-U6, TRB-16). */
	static constexpr int32 MinUdpMtuBytes = 280;
	/** Receiver: largest reassembled UDP message accepted, in bytes. */
	static constexpr TCHAR MaxFrameOptionKey[] = TEXT("udp.maxframe");

	/** Trim and normalise a hostname for logging / URI generation. */
	FString NormaliseHostname(const FString& Host);

	/** Utility for generating a human readable stream id (host:port). */
	FString ComposeStreamId(const FString& Host, int32 Port);

	/** "<Scheme>://host:port", with brackets around an IPv6 host. */
	FString MakeUri(const TCHAR* Scheme, const FString& Host, int32 Port);

	/** A port option: 1 to 65535 in digits, otherwise Default (TRB-26: "80abc" is not port 80). */
	int32 ReadPortOption(const FO3DTransportConfig& Config, const TCHAR* Key, int32 Default);

	/**
	 * The endpoint of a TCP or UDP config, parsed with O3DTransportOptions::ParseHostPort (strict
	 * port 1-65535, bracketed IPv6; TRB-26). Precedence, as before WP-A1 step 4: a "<Scheme>://"
	 * URI, then the host and port options, then a StreamId of the form host:port. The host is
	 * trimmed and lowercased. Pure parsing, any thread; nothing is resolved here.
	 */
	bool ParseEndpoint(const FO3DTransportConfig& Config, const TCHAR* Scheme, FO3DHostPort& OutEndpoint);

	/** Capabilities of the TCP transport (ADR 0007 item 4); the same for every config. Any thread. */
	FO3DTransportCapabilities GetTcpCapabilities(const FO3DTransportConfig& Config);

	/** Capabilities of the UDP transport (ADR 0007 item 4); the same for every config. Any thread. */
	FO3DTransportCapabilities GetUdpCapabilities(const FO3DTransportConfig& Config);
}
