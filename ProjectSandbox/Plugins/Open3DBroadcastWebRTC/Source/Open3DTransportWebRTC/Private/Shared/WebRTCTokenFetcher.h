// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "Templates/SharedPointer.h"
#include "WebRTCTokenManager.h"

/**
 * Performs asynchronous HTTP requests to fetch JWT tokens from a token generator endpoint.
 *
 * Lifetime (WP-S7, TRF-24): HTTP completion delegates and retry tickers hold the fetcher's
 * shared state weakly, never `this`. Retries are scheduled on the core ticker (FTSTicker),
 * so they do not depend on GWorld. CancelPendingRequests() unbinds every completion delegate
 * before cancelling its request and removes pending retry tickers, so OnComplete is not called
 * after it returns.
 *
 * Threading: FetchTokenAsync and CancelPendingRequests are called from the game thread. The
 * completion delegate may run on the game thread or the HTTP thread depending on the HTTP
 * module's delegate policy (needs-UE-verification); the shared state is locked either way.
 * Retry tickers run on the game thread.
 */
class FO3DTokenFetcher : public IO3DTokenFetcher
{
public:
	FO3DTokenFetcher();
	virtual ~FO3DTokenFetcher() override;

	FO3DTokenFetcher(const FO3DTokenFetcher&) = delete;
	FO3DTokenFetcher& operator=(const FO3DTokenFetcher&) = delete;

	/**
	 * Initiate an asynchronous token fetch request.
	 *
	 * @param Request Token fetch parameters
	 * @param OnComplete Callback invoked once when the request completes (success or failure)
	 */
	virtual void FetchTokenAsync(const FO3DTokenFetchRequest& Request, TFunction<void(const FO3DTokenResult&)> OnComplete) override;

	/**
	 * Cancel any pending requests and retries. OnComplete is not called for them.
	 */
	virtual void CancelPendingRequests() override;

	/** Build the JSON request body from fetch request parameters. */
	static FString BuildRequestBody(const FO3DTokenFetchRequest& Request);

	/** Parse the HTTP response to extract token and expiry. */
	static FO3DTokenResult ParseResponse(FHttpResponsePtr Response);

	/** Calculate exponential backoff delay in seconds. */
	static float CalculateBackoffDelay(int32 RetryAttempt);

	/** Determine if an error is retryable. */
	static bool IsRetryableError(const FO3DTokenResult& Result, int32 ResponseCode);

private:
	struct FState
	{
		FCriticalSection Mutex;
		/** Active HTTP requests (for cancellation) */
		TArray<FHttpRequestPtr> ActiveRequests;
		/** Pending retry tickers (for cancellation) */
		TArray<FTSTicker::FDelegateHandle> RetryTickers;
		/** Incremented by CancelPendingRequests(); work started under an older value is dropped. */
		uint64 Epoch = 0;
	};

	using FStateRef = TSharedRef<FState, ESPMode::ThreadSafe>;
	using FStateWeak = TWeakPtr<FState, ESPMode::ThreadSafe>;

	static void ExecuteFetchWithRetry(const FStateWeak& WeakState, uint64 Epoch, const FO3DTokenFetchRequest& Request,
		TFunction<void(const FO3DTokenResult&)> OnComplete, int32 RetryAttempt);

	/** Created in the constructor and never reassigned. */
	FStateRef State;
};
