// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Containers/UnrealString.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

DECLARE_LOG_CATEGORY_EXTERN(LogO3DWebRTCTokenManager, Log, All);

/**
 * Token acquisition modes
 */
enum class EO3DTokenMode : uint8
{
	/** Token manually provided by user */
	Manual,

	/** Token automatically fetched from endpoint */
	AutoFetch
};

/**
 * Token role type (Publisher or Subscriber)
 */
enum class EO3DTokenRole : uint8
{
	Publisher,
	Subscriber
};

/**
 * Configuration for token management
 */
struct FO3DTokenConfig
{
	/** Token acquisition mode */
	EO3DTokenMode Mode = EO3DTokenMode::Manual;

	/** Manual token (used when Mode == Manual) */
	FString ManualToken;

	/**
	 * Token endpoint URL for auto-fetch (used when Mode == AutoFetch). Must be https://, or
	 * http:// to localhost, 127.0.0.1 or ::1; anything else is refused (ADR 0004 item 6).
	 */
	FString EndpointUrl;

	/**
	 * Credential for the token endpoint, sent as "Authorization: Bearer <value>" when not empty.
	 * Comes from the `webrtc.tokenEndpointAuth` secret. Never logged.
	 */
	FString EndpointAuth;

	/** Room name for token generation */
	FString RoomName;

	/** Identity/participant name */
	FString Identity;

	/** Token role (Publisher or Subscriber) */
	EO3DTokenRole Role = EO3DTokenRole::Publisher;

	/** Seconds before expiry to trigger refresh (default: 300 = 5 minutes) */
	int32 RefreshLeadTimeSec = 300;
};

/**
 * Result of a token operation
 */
struct FO3DTokenResult
{
	/** Success flag */
	bool bSuccess = false;

	/** Error message (if any) */
	FString ErrorMessage;

	/** Retrieved token */
	FString Token;

	/** Token expiry timestamp (Unix seconds), 0 if unknown */
	int64 ExpiresAt = 0;
};

/**
 * Token fetch request parameters
 */
struct FO3DTokenFetchRequest
{
	/** Endpoint URL (e.g., https://livekit.example.com/token) */
	FString EndpointUrl;

	/** Room name */
	FString RoomName;

	/** Identity/participant name */
	FString Identity;

	/** Role: "publisher" or "subscriber" */
	FString Role;

	/** Request timeout in seconds (default: 10) */
	float TimeoutSeconds = 10.0f;

	/** Maximum number of retry attempts (default: 5) */
	int32 MaxRetries = 5;

	/**
	 * Sent as "Authorization: Bearer <value>" when not empty. Never logged.
	 * The request carries no grants: the endpoint decides them from the authenticated caller
	 * (ADR 0004 item 6, TRF-22).
	 */
	FString AuthBearer;
};

/**
 * Fetches tokens asynchronously. FO3DTokenFetcher is the HTTP implementation; tests inject a
 * fake through FO3DTokenFetcherFactory (WP-S7).
 *
 * OnComplete may run on any thread and must not be called after CancelPendingRequests()
 * returns.
 */
class IO3DTokenFetcher
{
public:
	virtual ~IO3DTokenFetcher() = default;
	virtual void FetchTokenAsync(const FO3DTokenFetchRequest& Request, TFunction<void(const FO3DTokenResult&)> OnComplete) = 0;
	virtual void CancelPendingRequests() = 0;
};

using FO3DTokenFetcherFactory = TFunction<TSharedRef<IO3DTokenFetcher, ESPMode::ThreadSafe>()>;

/**
 * Manages JWT token lifecycle for WebRTC transport.
 *
 * Responsibilities:
 * - Store and provide current valid token
 * - Parse JWT expiry from token payload
 * - Track token expiration
 * - Run asynchronous token fetches (auto-fetch mode)
 *
 * Threading (WP-S7, TRF-3/TRF-15/TRF-24): all public methods are thread-safe. Fetch results
 * are stored in a shared state object that the fetch callback holds weakly, so a result that
 * arrives after Reset() or destruction is dropped. The WebRTC sender and receiver never pass a
 * callback that captures themselves; they poll GetCurrentToken()/IsRefreshInProgress() from the
 * game thread and compare token generations to detect a new token.
 */
class FO3DTokenManager
{
public:
	/** @param InFetcherFactory Creates the fetcher for auto-fetch mode. Null uses the HTTP fetcher. */
	explicit FO3DTokenManager(FO3DTokenFetcherFactory InFetcherFactory = nullptr);
	~FO3DTokenManager();

	FO3DTokenManager(const FO3DTokenManager&) = delete;
	FO3DTokenManager& operator=(const FO3DTokenManager&) = delete;

	/**
	 * Initialize the token manager with configuration.
	 *
	 * @param InConfig Token configuration
	 * @return true if initialization succeeded
	 */
	bool Initialize(const FO3DTokenConfig& InConfig);

	/**
	 * Get the current token (thread-safe).
	 * An expired token is logged once per token, not on every call (TRF-23).
	 *
	 * @param OutToken Receives the current token
	 * @param OutGeneration Optional; receives the generation of the returned token
	 * @return true if a valid, unexpired token is available
	 */
	bool GetCurrentToken(FString& OutToken, uint64* OutGeneration = nullptr) const;

	/** Increments each time a new token is stored (manual token or successful fetch). 0 = none yet. */
	uint64 GetTokenGeneration() const;

	/** True while an auto-fetch request is outstanding. */
	bool IsRefreshInProgress() const;

	/** True when configured for auto-fetch. */
	bool IsAutoFetch() const;

	/**
	 * Check if the current token is expired.
	 *
	 * @return true if token is expired or about to expire
	 */
	bool IsTokenExpired() const;

	/**
	 * Check if token needs refresh based on lead time.
	 *
	 * @return true if token should be refreshed proactively
	 */
	bool NeedsRefresh() const;

	/**
	 * Trigger asynchronous token refresh. Does nothing if a refresh is already in progress
	 * (the callback, if any, is queued and called when that refresh completes).
	 *
	 * @param OnComplete Callback when refresh completes (optional, any thread)
	 */
	void RefreshTokenAsync(TFunction<void(const FO3DTokenResult&)> OnComplete = nullptr);

	/** Cancels an outstanding fetch (used for the fetch timeout). Pending callbacks are dropped. */
	void CancelRefresh();

	/**
	 * Get time until token expires (in seconds).
	 *
	 * @return Seconds until expiry, 0 if expired, -1 if expiry unknown
	 */
	int64 GetTimeUntilExpiry() const;

	/**
	 * Reset the token manager (clears current token, cancels any fetch).
	 */
	void Reset();

private:
	/** State shared with in-flight fetch callbacks (held weakly by them). Guarded by Mutex. */
	struct FState
	{
		mutable FCriticalSection Mutex;
		FO3DTokenConfig Config;
		FString CurrentToken;
		int64 TokenExpiresAt = 0;
		bool bTokenValid = false;
		uint64 TokenGeneration = 0;
		/** Set once the expiry of the current token has been logged (reset per token). */
		mutable bool bExpiryLogged = false;
		bool bRefreshInProgress = false;
		/** Incremented by Reset()/CancelRefresh(); a fetch started under an older value is ignored. */
		uint64 FetchEpoch = 0;
		TArray<TFunction<void(const FO3DTokenResult&)>> PendingCallbacks;
	};

	/** Parse JWT token to extract expiry timestamp */
	static int64 ParseJwtExpiry(const FString& JwtToken);

	/** Get current Unix timestamp in seconds */
	static int64 GetCurrentUnixTime();

	/** Handle completion of async token fetch */
	static void OnTokenFetchComplete(const TSharedRef<FState, ESPMode::ThreadSafe>& State, uint64 FetchEpoch, const FO3DTokenResult& Result);

	FO3DTokenFetcherFactory FetcherFactory;

	/** Created in the constructor and never reassigned. */
	TSharedRef<FState, ESPMode::ThreadSafe> State;

	/** Token fetcher (for auto-fetch mode). Game thread only. */
	TSharedPtr<IO3DTokenFetcher, ESPMode::ThreadSafe> TokenFetcher;
};
