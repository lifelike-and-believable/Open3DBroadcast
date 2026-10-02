// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"
#include "Transport/O3DTransportOptionsView.h"

class FInternetAddr;

/*
 * Shared option parsing for transports (ADR 0007 item 7, WP-A1 step 4; TRB-26, TRB-38, SHR-9).
 *
 * Typed getters over a transport's options, and one strict host:port parser in place of the
 * per-transport copies. The getters take an FO3DTransportOptionsView (WP-A1 PR 5a), which a plain
 * map converts to, so FO3DTransportConfig::AdvancedParams can be passed as it is. The caller's
 * Default always wins over a schema default here; FO3DTransportOptionsView's own getters are the
 * ones that use the schema's. Keys are matched case-insensitively.
 *
 * Threading: everything is a pure function and may be called on any thread, except
 * ResolveHostPort, which may block on DNS and therefore belongs on a transport worker, never on
 * the game thread (ADR 0007 open question 3's default).
 */

/** A parsed endpoint. */
struct FO3DHostPort
{
	/** Host name or address without brackets ("192.168.1.10", "mocap-pc.local", "::1", "*"). */
	FString Host;
	/** 1 to 65535. */
	int32 Port = 0;
	/** True when Host is an IPv6 literal (it was bracketed, or bare with more than one colon). */
	bool bIPv6 = false;

	/** "host:port", with brackets around an IPv6 host ("[::1]:9000"). */
	OPEN3DSHARED_API FString ToString() const;
};

namespace O3DTransportOptions
{
	/** The value of Key (case-insensitive), or null. */
	OPEN3DSHARED_API const FString* Find(const FO3DTransportOptionsView& Options, const FString& Key);

	/** The trimmed value of Key, or Default when absent or empty. */
	OPEN3DSHARED_API FString GetString(const FO3DTransportOptionsView& Options, const FString& Key, const FString& Default = FString());

	/**
	 * Strict integer parse: optional sign and decimal digits only, surrounding whitespace allowed,
	 * nothing else ("80abc", "", "1e3" and overflowing values fail; TRB-26).
	 */
	OPEN3DSHARED_API bool TryParseInt(const FString& Text, int64& OutValue);

	/** Key as an integer clamped to [Min, Max]; Default (unclamped) when absent or not an integer. */
	OPEN3DSHARED_API int32 GetInt(const FO3DTransportOptionsView& Options, const FString& Key, int32 Default, int32 Min = MIN_int32, int32 Max = MAX_int32);

	/** Strict decimal number ("1.5", "-2", "1e-3"); NaN and infinities fail. */
	OPEN3DSHARED_API bool TryParseDouble(const FString& Text, double& OutValue);

	/** Key as a number clamped to [Min, Max]; Default when absent or not a number. */
	OPEN3DSHARED_API double GetDouble(const FO3DTransportOptionsView& Options, const FString& Key, double Default, double Min = TNumericLimits<double>::Lowest(), double Max = TNumericLimits<double>::Max());

	/** true/false, 1/0, yes/no, on/off (any case, surrounding whitespace allowed). False for anything else. */
	OPEN3DSHARED_API bool TryParseBool(const FString& Text, bool& OutValue);

	/** true/false, 1/0, yes/no, on/off (any case); Default when absent or anything else. */
	OPEN3DSHARED_API bool GetBool(const FO3DTransportOptionsView& Options, const FString& Key, bool Default);

	/** Strict port: decimal digits only, 1 to 65535. */
	OPEN3DSHARED_API bool TryParsePort(const FString& Text, int32& OutPort);

	/**
	 * Parses an endpoint (TRB-26, SHR-9). Accepted forms, each with an optional "scheme://" prefix
	 * and an optional path or query after the port ("tcp://host:9000/x?y=1"):
	 *   host:port, 192.168.1.10:port, [IPv6]:port, and, when DefaultPort is 1 to 65535, the same
	 *   without ":port" (host, 192.168.1.10, [IPv6], bare IPv6 such as ::1).
	 * Refused: an empty host, a missing port without DefaultPort, a port that is not 1 to 65535 or
	 * has trailing characters, an unclosed bracket, characters other than letters, digits, '.',
	 * '-', '_' in a host name, and user info ("user@host"). A dotted address without a port is
	 * never rewritten (the SHR-9 bug of the deleted NormalizeTcpUrlHostPort).
	 * On failure OutEndpoint is untouched and OutError, when given, says why.
	 */
	OPEN3DSHARED_API bool ParseHostPort(const FString& Input, FO3DHostPort& OutEndpoint, int32 DefaultPort = 0, FString* OutError = nullptr);

	/**
	 * Resolves an endpoint to a socket address with the platform socket subsystem: IPv4 and IPv6
	 * literals and host names (DNS; TRB-26). "*" and an empty host give the IPv4 any-address, for
	 * binding. The first address the resolver returns is used; its protocol family tells the
	 * caller which socket type to create. May block on DNS: call it on a transport worker, never
	 * on the game thread. False, with OutError, when the subsystem is missing or the name does
	 * not resolve.
	 */
	OPEN3DSHARED_API bool ResolveHostPort(const FO3DHostPort& Endpoint, TSharedPtr<FInternetAddr>& OutAddress, FString* OutError = nullptr);

	/**
	 * True when Endpoint.Host is an IP literal: an IPv6 address (bIPv6) or a dotted-quad IPv4
	 * address (four decimal parts 0 to 255). ResolveHostPort needs no DNS for these, so a caller on
	 * the game thread may resolve them there (a listen or bind address). Wildcards ("*", empty) are
	 * not literals. Any thread.
	 */
	OPEN3DSHARED_API bool IsIpLiteral(const FO3DHostPort& Endpoint);
}
