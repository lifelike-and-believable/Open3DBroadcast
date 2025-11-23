#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Containers/UnrealString.h"

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
	
	/** Token endpoint URL for auto-fetch (used when Mode == AutoFetch) */
	FString EndpointUrl;
	
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

// Forward declaration
class FO3DTokenFetcher;

/**
 * Manages JWT token lifecycle for WebRTC transport.
 * 
 * Responsibilities:
 * - Store and provide current valid token
 * - Parse JWT expiry from token payload
 * - Track token expiration
 * - Coordinate automatic token refresh
 * - Thread-safe token access
 * 
 * Thread Safety: All public methods are thread-safe via mutex.
 */
class FO3DTokenManager
{
public:
	FO3DTokenManager();
	~FO3DTokenManager();

	/**
	 * Initialize the token manager with configuration.
	 * 
	 * @param InConfig Token configuration
	 * @return true if initialization succeeded
	 */
	bool Initialize(const FO3DTokenConfig& InConfig);

	/**
	 * Get the current token (thread-safe).
	 * 
	 * @param OutToken Receives the current token
	 * @return true if a valid token is available
	 */
	bool GetCurrentToken(FString& OutToken) const;

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
	 * Trigger asynchronous token refresh.
	 * Should be called when NeedsRefresh() returns true.
	 * 
	 * @param OnComplete Callback when refresh completes (optional)
	 */
	void RefreshTokenAsync(TFunction<void(const FO3DTokenResult&)> OnComplete = nullptr);

	/**
	 * Get time until token expires (in seconds).
	 * 
	 * @return Seconds until expiry, 0 if expired, -1 if expiry unknown
	 */
	int64 GetTimeUntilExpiry() const;

	/**
	 * Reset the token manager (clears current token).
	 */
	void Reset();

private:
	/** Parse JWT token to extract expiry timestamp */
	int64 ParseJwtExpiry(const FString& JwtToken) const;

	/** Get current Unix timestamp in seconds */
	int64 GetCurrentUnixTime() const;

	/** Handle completion of async token fetch */
	void OnTokenFetchComplete(const FO3DTokenResult& Result);

	/** Configuration */
	FO3DTokenConfig Config;

	/** Current token (protected by mutex) */
	mutable FCriticalSection TokenMutex;
	FString CurrentToken;
	int64 TokenExpiresAt = 0;
	bool bTokenValid = false;

	/** Token fetcher (for auto-fetch mode) */
	TUniquePtr<FO3DTokenFetcher> TokenFetcher;

	/** Pending refresh callbacks */
	mutable FCriticalSection CallbackMutex;
	TArray<TFunction<void(const FO3DTokenResult&)>> PendingCallbacks;
	bool bRefreshInProgress = false;
};
