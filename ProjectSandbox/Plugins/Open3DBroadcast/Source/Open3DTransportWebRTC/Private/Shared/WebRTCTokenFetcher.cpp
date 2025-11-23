#include "WebRTCTokenFetcher.h"
#include "WebRTCTokenManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Logging/LogMacros.h"

FO3DTokenFetcher::FO3DTokenFetcher()
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

	// Create HTTP request
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
	
	HttpRequest->SetVerb(TEXT("POST"));
	HttpRequest->SetURL(Request.EndpointUrl);
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	HttpRequest->SetTimeout(Request.TimeoutSeconds);

	// Add authentication header if API key provided
	if (!Request.ApiKey.IsEmpty())
	{
		const FString AuthHeader = FString::Printf(TEXT("Bearer %s"), *Request.ApiKey);
		HttpRequest->SetHeader(TEXT("Authorization"), AuthHeader);
	}

	// Build request body
	FString RequestBody = BuildRequestBody(Request);
	HttpRequest->SetContentAsString(RequestBody);

	UE_LOG(LogO3DWebRTCTokenManager, Verbose, TEXT("Sending token fetch request to: %s"), *Request.EndpointUrl);
	UE_LOG(LogO3DWebRTCTokenManager, Verbose, TEXT("Request body: %s"), *RequestBody);

	// Bind completion callback
	HttpRequest->OnProcessRequestComplete().BindLambda([this, OnComplete](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSuccess)
	{
		OnHttpRequestComplete(Req, Resp, bSuccess, OnComplete);
	});

	// Send request
	if (!HttpRequest->ProcessRequest())
	{
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Failed to initiate HTTP request"));
		
		FO3DTokenResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Failed to initiate HTTP request");
		
		if (OnComplete)
		{
			OnComplete(Result);
		}
		return;
	}

	// Track active request
	ActiveRequests.Add(HttpRequest);
}

void FO3DTokenFetcher::CancelPendingRequests()
{
	for (FHttpRequestPtr& Request : ActiveRequests)
	{
		if (Request.IsValid() && Request->GetStatus() == EHttpRequestStatus::Processing)
		{
			Request->CancelRequest();
		}
	}
	
	ActiveRequests.Empty();
}

FString FO3DTokenFetcher::BuildRequestBody(const FO3DTokenFetchRequest& Request) const
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

void FO3DTokenFetcher::OnHttpRequestComplete(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful, TFunction<void(const FO3DTokenResult&)> OnComplete)
{
	// Remove from active requests
	ActiveRequests.Remove(Request);

	FO3DTokenResult Result;

	if (!bWasSuccessful)
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("HTTP request failed (network error or timeout)");
		
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("%s"), *Result.ErrorMessage);
		
		if (OnComplete)
		{
			OnComplete(Result);
		}
		return;
	}

	if (!Response.IsValid())
	{
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Invalid HTTP response");
		
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("%s"), *Result.ErrorMessage);
		
		if (OnComplete)
		{
			OnComplete(Result);
		}
		return;
	}

	const int32 ResponseCode = Response->GetResponseCode();
	
	if (ResponseCode < 200 || ResponseCode >= 300)
	{
		Result.bSuccess = false;
		Result.ErrorMessage = FString::Printf(TEXT("HTTP error %d: %s"), ResponseCode, *Response->GetContentAsString());
		
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Token fetch failed: %s"), *Result.ErrorMessage);
		
		if (OnComplete)
		{
			OnComplete(Result);
		}
		return;
	}

	// Parse response
	Result = ParseResponse(Response);
	
	if (Result.bSuccess)
	{
		UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token fetched successfully (expires at: %lld)"), Result.ExpiresAt);
	}
	else
	{
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Failed to parse token response: %s"), *Result.ErrorMessage);
	}

	if (OnComplete)
	{
		OnComplete(Result);
	}
}

FO3DTokenResult FO3DTokenFetcher::ParseResponse(FHttpResponsePtr Response) const
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
