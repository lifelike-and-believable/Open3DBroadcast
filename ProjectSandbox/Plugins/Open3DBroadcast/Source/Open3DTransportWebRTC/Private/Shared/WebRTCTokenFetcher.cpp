#include "WebRTCTokenFetcher.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Logging/LogMacros.h"

FO3DTokenFetcher::FO3DTokenFetcher()
	: State(MakeShared<FState, ESPMode::ThreadSafe>())
{
}

FO3DTokenFetcher::~FO3DTokenFetcher()
{
	CancelPendingRequests();
}

void FO3DTokenFetcher::FetchTokenAsync(const FO3DTokenFetchRequest& Request, TFunction<void(const FO3DTokenResult&)> OnComplete)
{
	// Validate request
	if (Request.EndpointUrl.IsEmpty())
	{
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Token fetch failed: Endpoint URL is empty"));

		FO3DTokenResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Endpoint URL is empty");

		if (OnComplete)
		{
			OnComplete(Result);
		}
		return;
	}

	uint64 Epoch = 0;
	{
		FScopeLock Lock(&State->Mutex);
		Epoch = State->Epoch;
	}

	// Start fetch with retry logic (attempt 0 is the initial attempt)
	ExecuteFetchWithRetry(FStateWeak(State), Epoch, Request, MoveTemp(OnComplete), 0);
}

void FO3DTokenFetcher::ExecuteFetchWithRetry(const FStateWeak& WeakState, uint64 Epoch, const FO3DTokenFetchRequest& Request,
	TFunction<void(const FO3DTokenResult&)> OnComplete, int32 RetryAttempt)
{
	const TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakState.Pin();
	if (!Pinned.IsValid())
	{
		return;
	}

	// Create HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();

	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetURL(Request.EndpointUrl);
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetTimeout(Request.TimeoutSeconds);

	// Build request body
	const FString RequestBody = BuildRequestBody(Request);
	HttpRequest->SetContentAsString(RequestBody);

	if (RetryAttempt == 0)
	{
		UE_LOG(LogO3DWebRTCTokenManager, Verbose, TEXT("Sending token fetch request to: %s"), *Request.EndpointUrl);
	}
	else
	{
		UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Retrying token fetch (attempt %d/%d) to: %s"),
			RetryAttempt, Request.MaxRetries, *Request.EndpointUrl);
	}

	// The delegate holds the state weakly (TRF-24). It never touches the fetcher object.
	HttpRequest->OnProcessRequestComplete().BindLambda([WeakState, Epoch, Request, OnComplete, RetryAttempt](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		const TSharedPtr<FState, ESPMode::ThreadSafe> Shared = WeakState.Pin();
		if (!Shared.IsValid())
		{
			return;
		}

		{
			FScopeLock Lock(&Shared->Mutex);
			Shared->ActiveRequests.Remove(Req);
			if (Shared->Epoch != Epoch)
			{
				return; // cancelled
			}
		}

		FO3DTokenResult Result;
		int32 ResponseCode = 0;

		if (!bSuccess)
		{
			Result.bSuccess = false;
			Result.ErrorMessage = TEXT("HTTP request failed (network error or timeout)");
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("%s"), *Result.ErrorMessage);
		}
		else if (!Resp.IsValid())
		{
			Result.bSuccess = false;
			Result.ErrorMessage = TEXT("Invalid HTTP response");
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("%s"), *Result.ErrorMessage);
		}
		else
		{
			ResponseCode = Resp->GetResponseCode();

			if (ResponseCode < 200 || ResponseCode >= 300)
			{
				Result.bSuccess = false;
				Result.ErrorMessage = FString::Printf(TEXT("HTTP error %d: %s"), ResponseCode, *Resp->GetContentAsString());
				UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Token fetch failed: %s"), *Result.ErrorMessage);
			}
			else
			{
				// Parse response
				Result = ParseResponse(Resp);

				if (Result.bSuccess)
				{
					UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token fetched successfully (expires at: %lld)"), Result.ExpiresAt);
				}
				else
				{
					UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Failed to parse token response: %s"), *Result.ErrorMessage);
				}
			}
		}

		// Check if we should retry
		if (!Result.bSuccess && IsRetryableError(Result, ResponseCode) && RetryAttempt < Request.MaxRetries)
		{
			const float DelaySeconds = CalculateBackoffDelay(RetryAttempt);
			UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Will retry after %.1f seconds..."), DelaySeconds);

			// Core ticker instead of a world timer: independent of GWorld and PIE (TRF-24).
			// The ticker delegate runs once (returns false) and holds the state weakly.
			FScopeLock Lock(&Shared->Mutex);
			if (Shared->Epoch != Epoch)
			{
				return;
			}
			const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([WeakState, Epoch, Request, OnComplete, RetryAttempt](float /*DeltaTime*/)
				{
					ExecuteFetchWithRetry(WeakState, Epoch, Request, OnComplete, RetryAttempt + 1);
					return false;
				}),
				DelaySeconds);
			Shared->RetryTickers.Add(Handle);
			return;
		}

		if (!Result.bSuccess && RetryAttempt >= Request.MaxRetries)
		{
			UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Token fetch failed after %d attempts: %s"),
				RetryAttempt + 1, *Result.ErrorMessage);
		}

		if (OnComplete)
		{
			OnComplete(Result);
		}
	});

	{
		FScopeLock Lock(&Pinned->Mutex);
		if (Pinned->Epoch != Epoch)
		{
			// Cancelled while this retry was pending.
			HttpRequest->OnProcessRequestComplete().Unbind();
			return;
		}
		Pinned->ActiveRequests.Add(HttpRequest);
	}

	// Send request
	if (!HttpRequest->ProcessRequest())
	{
		// Unbind first so a delegate fired by the failed request cannot report a second result.
		HttpRequest->OnProcessRequestComplete().Unbind();
		{
			FScopeLock Lock(&Pinned->Mutex);
			Pinned->ActiveRequests.Remove(HttpRequest);
		}

		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Failed to initiate HTTP request"));

		FO3DTokenResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Failed to initiate HTTP request");

		if (OnComplete)
		{
			OnComplete(Result);
		}
	}
}

void FO3DTokenFetcher::CancelPendingRequests()
{
	TArray<FHttpRequestPtr> Requests;
	TArray<FTSTicker::FDelegateHandle> Tickers;
	{
		FScopeLock Lock(&State->Mutex);
		++State->Epoch;
		Requests = MoveTemp(State->ActiveRequests);
		State->ActiveRequests.Reset();
		Tickers = MoveTemp(State->RetryTickers);
		State->RetryTickers.Reset();
	}

	for (FTSTicker::FDelegateHandle& Handle : Tickers)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Handle);
	}

	for (FHttpRequestPtr& Request : Requests)
	{
		if (Request.IsValid())
		{
			// Unbind before cancelling: CancelRequest may still fire the delegate (TRF-24).
			Request->OnProcessRequestComplete().Unbind();
			Request->CancelRequest();
		}
	}
}

FString FO3DTokenFetcher::BuildRequestBody(const FO3DTokenFetchRequest& Request)
{
	// Build JSON request body
	TSharedPtr<FJsonObject> JsonObject = MakeShareable(new FJsonObject());

	// Required fields
	if (!Request.RoomName.IsEmpty())
	{
		JsonObject->SetStringField(TEXT("room"), Request.RoomName);
	}

	if (!Request.Identity.IsEmpty())
	{
		JsonObject->SetStringField(TEXT("identity"), Request.Identity);
	}

	if (!Request.Role.IsEmpty())
	{
		JsonObject->SetStringField(TEXT("role"), Request.Role);
	}

	// Grants object
	TSharedPtr<FJsonObject> GrantsObject = MakeShareable(new FJsonObject());

	// Default grants based on role
	if (Request.Role.Equals(TEXT("publisher"), ESearchCase::IgnoreCase))
	{
		GrantsObject->SetBoolField(TEXT("roomCreate"), true);
		GrantsObject->SetBoolField(TEXT("canPublish"), true);
		GrantsObject->SetBoolField(TEXT("canSubscribe"), false);
	}
	else if (Request.Role.Equals(TEXT("subscriber"), ESearchCase::IgnoreCase))
	{
		GrantsObject->SetBoolField(TEXT("roomCreate"), false);
		GrantsObject->SetBoolField(TEXT("canPublish"), false);
		GrantsObject->SetBoolField(TEXT("canSubscribe"), true);
	}

	// Additional grants (can override defaults)
	for (const auto& Grant : Request.AdditionalGrants)
	{
		const FString& Key = Grant.Key;
		const FString& Value = Grant.Value;

		// Try to parse as boolean
		if (Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			GrantsObject->SetBoolField(Key, true);
		}
		else if (Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			GrantsObject->SetBoolField(Key, false);
		}
		else
		{
			// Store as string
			GrantsObject->SetStringField(Key, Value);
		}
	}

	JsonObject->SetObjectField(TEXT("grants"), GrantsObject);

	// Serialize to string
	FString OutputString;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&OutputString);

	if (FJsonSerializer::Serialize(JsonObject.ToSharedRef(), JsonWriter))
	{
		return OutputString;
	}

	UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Failed to serialize JSON request body"));
	return TEXT("{}");
}

float FO3DTokenFetcher::CalculateBackoffDelay(int32 RetryAttempt)
{
	// Exponential backoff: 1s, 2s, 4s, 8s, 16s
	// Formula: delay = 2^RetryAttempt seconds, capped at 16 seconds
	// Clamp RetryAttempt to prevent integer overflow in bit shift
	const int32 MaxDelay = 16;
	const int32 Delay = FMath::Min(1 << FMath::Min(RetryAttempt, 4), MaxDelay);
	return static_cast<float>(Delay);
}

bool FO3DTokenFetcher::IsRetryableError(const FO3DTokenResult& Result, int32 ResponseCode)
{
	// Retry on network errors and timeouts (no response code)
	if (Result.ErrorMessage.Contains(TEXT("network error"), ESearchCase::IgnoreCase) ||
		Result.ErrorMessage.Contains(TEXT("timeout"), ESearchCase::IgnoreCase))
	{
		return true;
	}

	// Check HTTP response code for retryable errors
	// HTTP 5xx server errors are retryable
	if (ResponseCode >= 500 && ResponseCode < 600)
	{
		return true;
	}

	// HTTP 429 (Too Many Requests) is also retryable
	if (ResponseCode == 429)
	{
		return true;
	}

	// Don't retry client errors (4xx) or other errors
	return false;
}

FO3DTokenResult FO3DTokenFetcher::ParseResponse(FHttpResponsePtr Response)
{
	FO3DTokenResult Result;

	if (!Response.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Invalid response");
		return Result;
	}

	const FString ResponseBody = Response->GetContentAsString();

	if (ResponseBody.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Empty response body");
		return Result;
	}

	// Parse JSON
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ResponseBody);

	if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Failed to parse JSON response");
		UE_LOG(LogO3DWebRTCTokenManager, Verbose, TEXT("Response body: %s"), *ResponseBody);
		return Result;
	}

	// Extract token
	FString Token;
	if (!JsonObject->TryGetStringField(TEXT("token"), Token) || Token.IsEmpty())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Response missing 'token' field");
		return Result;
	}

	Result.Token = Token;
	Result.bSuccess = true;

	// Extract expiry (optional fields)
	// Try "expiresAt" (Unix timestamp in seconds)
	int64 ExpiresAt = 0;
	if (JsonObject->TryGetNumberField(TEXT("expiresAt"), ExpiresAt))
	{
		Result.ExpiresAt = ExpiresAt;
	}
	// Try "ttl" (time-to-live in seconds)
	else
	{
		int64 Ttl = 0;
		if (JsonObject->TryGetNumberField(TEXT("ttl"), Ttl))
		{
			// Convert TTL to absolute timestamp
			FDateTime Now = FDateTime::UtcNow();
			Result.ExpiresAt = Now.ToUnixTimestamp() + Ttl;
		}
	}

	return Result;
}
