#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Containers/UnrealString.h"
#include "TimerManager.h"

// Forward declarations
struct FO3DTokenResult;

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
	
	/** Additional grants for token generation (optional) */
	TMap<FString, FString> AdditionalGrants;
};

/**
 * Performs asynchronous HTTP requests to fetch JWT tokens from a token generator endpoint.
 * 
 * Thread Safety: Safe to call from any thread. HTTP callbacks execute on HTTP module's thread.
 * 
 * Usage:
 *   FO3DTokenFetcher Fetcher;
 *   Fetcher.FetchTokenAsync(Request, [](const FO3DTokenResult& Result) {
 *       if (Result.bSuccess) {
 *           // Use Result.Token
 *       }
 *   });
 */
class FO3DTokenFetcher
{
public:
	FO3DTokenFetcher();
	~FO3DTokenFetcher();

	/**
	 * Initiate an asynchronous token fetch request.
	 * 
	 * @param Request Token fetch parameters
	 * @param OnComplete Callback invoked when request completes (success or failure)
	 */
	void FetchTokenAsync(const FO3DTokenFetchRequest& Request, TFunction<void(const FO3DTokenResult&)> OnComplete);

	/**
	 * Cancel any pending requests (called during shutdown).
	 */
	void CancelPendingRequests();

private:
	/**
	 * Build the JSON request body from fetch request parameters.
	 */
	FString BuildRequestBody(const FO3DTokenFetchRequest& Request) const;

	/**
	 * Parse the HTTP response to extract token and expiry.
	 */
	FO3DTokenResult ParseResponse(FHttpResponsePtr Response) const;

	/**
	 * Execute a fetch request with retry context.
	 */
	void ExecuteFetchWithRetry(const FO3DTokenFetchRequest& Request, TFunction<void(const FO3DTokenResult&)> OnComplete, int32 RetryAttempt);

	/**
	 * Calculate exponential backoff delay in seconds.
	 */
	float CalculateBackoffDelay(int32 RetryAttempt) const;

	/**
	 * Determine if an error is retryable.
	 */
	bool IsRetryableError(const FO3DTokenResult& Result, int32 ResponseCode) const;

	/** Active HTTP requests (for cancellation) */
	TArray<FHttpRequestPtr> ActiveRequests;
	
	/** Active retry timers (for cancellation during shutdown) */
	TArray<FTimerHandle> ActiveRetryTimers;
};
