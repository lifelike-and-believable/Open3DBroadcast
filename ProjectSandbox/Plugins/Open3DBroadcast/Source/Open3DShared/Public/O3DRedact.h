// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

/**
 * Log redaction helpers (ADR 0004 item 7).
 *
 * Declared secret option keys are never logged at all; these helpers are the defence in depth
 * for everything else that reaches a log line or a debug string. The key pattern here is only
 * used to decide what a log shows. It is never used to decide what is persisted: persistence is
 * decided by the secret keys a transport declares (FO3DTransportRegistry::GetSecretDeclaration:
 * its schema's Secret entries).
 */
namespace O3DRedact
{
	/** Placeholder printed instead of a redacted value. */
	inline const TCHAR* Marker() { return TEXT("<redacted>"); }

	/**
	 * True when Key looks like it names a credential: it contains, case-insensitively, one of
	 * token, secret, password, passwd, auth, credential, jwt, apikey or api_key, or it ends in
	 * ".key" or "_key". Over-matching is accepted.
	 */
	OPEN3DSHARED_API bool IsSensitiveKey(const FString& Key);

	/** Returns "<redacted>" when IsSensitiveKey(Key), otherwise Value unchanged. */
	OPEN3DSHARED_API FString Value(const FString& Key, const FString& Value);

	/**
	 * Returns Url with its scheme, host, port and path kept, and with user-info, every query
	 * value and the fragment replaced by "<redacted>". Query parameter names are kept so a log
	 * still shows which parameters were set. A string without a query, fragment or user-info
	 * is returned unchanged.
	 *
	 * Examples:
	 *   "wss://relay.example.com:443/moq?jwt=abc&x=1" -> "wss://relay.example.com:443/moq?jwt=<redacted>&x=<redacted>"
	 *   "https://user:pw@host/token#frag"            -> "https://<redacted>@host/token#<redacted>"
	 */
	OPEN3DSHARED_API FString Url(const FString& Url);
}
