#include "WebRTCTokenManager.h"
#include "WebRTCTokenFetcher.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY(LogO3DWebRTCTokenManager);

FO3DTokenManager::FO3DTokenManager()
{
}

FO3DTokenManager::~FO3DTokenManager()
{
	Reset();
}

bool FO3DTokenManager::Initialize(const FO3DTokenConfig& InConfig)
{
	FScopeLock Lock(&TokenMutex);
	
	Config = InConfig;
	bTokenValid = false;
	CurrentToken.Empty();
	TokenExpiresAt = 0;

	if (Config.Mode == EO3DTokenMode::Manual)
	{
		// Manual mode: use provided token directly
		if (Config.ManualToken.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Manual token mode but no token provided"));
			return false;
		}

		CurrentToken = Config.ManualToken;
		TokenExpiresAt = ParseJwtExpiry(CurrentToken);
		bTokenValid = true;

		UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token manager initialized (Manual mode)"));
		return true;
	}
	else if (Config.Mode == EO3DTokenMode::AutoFetch)
	{
		// Auto-fetch mode: validate configuration and create fetcher
		if (Config.EndpointUrl.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Auto-fetch mode but no endpoint URL provided"));
			return false;
		}

		if (Config.RoomName.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Auto-fetch mode but no room name provided"));
		}

		if (Config.Identity.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Auto-fetch mode but no identity provided"));
		}

		// Create token fetcher
		TokenFetcher = MakeUnique<FO3DTokenFetcher>();

		UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token manager initialized (Auto-fetch mode, endpoint: %s)"), *Config.EndpointUrl);
		return true;
	}

	return false;
}

bool FO3DTokenManager::GetCurrentToken(FString& OutToken) const
{
	FScopeLock Lock(&TokenMutex);
	
	if (!bTokenValid || CurrentToken.IsEmpty())
	{
		return false;
	}

	// Check if token is expired
	if (TokenExpiresAt > 0)
	{
		const int64 Now = GetCurrentUnixTime();
		if (Now >= TokenExpiresAt)
		{
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Current token is expired"));
			return false;
		}
	}

	OutToken = CurrentToken;
	return true;
}

bool FO3DTokenManager::IsTokenExpired() const
{
	FScopeLock Lock(&TokenMutex);
	
	if (!bTokenValid || TokenExpiresAt == 0)
	{
		return true; // No token or unknown expiry = treat as expired
	}

	const int64 Now = GetCurrentUnixTime();
	return Now >= TokenExpiresAt;
}

bool FO3DTokenManager::NeedsRefresh() const
{
	FScopeLock Lock(&TokenMutex);
	
	// Manual mode never needs refresh
	if (Config.Mode == EO3DTokenMode::Manual)
	{
		return false;
	}

	// If no token, definitely need to fetch
	if (!bTokenValid || CurrentToken.IsEmpty())
	{
		return true;
	}

	// If expiry unknown, don't refresh automatically
	if (TokenExpiresAt == 0)
	{
		return false;
	}

	// Check if within refresh lead time
	const int64 Now = GetCurrentUnixTime();
	const int64 TimeUntilExpiry = TokenExpiresAt - Now;
	
	return TimeUntilExpiry <= Config.RefreshLeadTimeSec;
}

void FO3DTokenManager::RefreshTokenAsync(TFunction<void(const FO3DTokenResult&)> OnComplete)
{
	// Check if already refreshing
	{
		FScopeLock Lock(&CallbackMutex);
		if (bRefreshInProgress)
		{
			UE_LOG(LogO3DWebRTCTokenManager, Verbose, TEXT("Token refresh already in progress, queuing callback"));
			if (OnComplete)
			{
				PendingCallbacks.Add(OnComplete);
			}
			return;
		}
		
		bRefreshInProgress = true;
		if (OnComplete)
		{
			PendingCallbacks.Add(OnComplete);
		}
	}

	// Validate configuration
	if (Config.Mode != EO3DTokenMode::AutoFetch)
	{
		UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("RefreshTokenAsync called but not in auto-fetch mode"));
		
		FO3DTokenResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Token manager not in auto-fetch mode");
		OnTokenFetchComplete(Result);
		return;
	}

	if (!TokenFetcher.IsValid())
	{
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Token fetcher not initialized"));
		
		FO3DTokenResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Token fetcher not initialized");
		OnTokenFetchComplete(Result);
		return;
	}

	// Prepare fetch request
	FO3DTokenFetchRequest Request;
	Request.EndpointUrl = Config.EndpointUrl;
	Request.RoomName = Config.RoomName;
	Request.Identity = Config.Identity;
	Request.Role = Config.Role == EO3DTokenRole::Publisher ? TEXT("publisher") : TEXT("subscriber");

	// Initiate async fetch
	UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Fetching token from endpoint: %s (room: %s, identity: %s, role: %s)"),
		*Request.EndpointUrl, *Request.RoomName, *Request.Identity, *Request.Role);

	TokenFetcher->FetchTokenAsync(Request, [this](const FO3DTokenResult& Result)
	{
		OnTokenFetchComplete(Result);
	});
}

int64 FO3DTokenManager::GetTimeUntilExpiry() const
{
	FScopeLock Lock(&TokenMutex);
	
	if (TokenExpiresAt == 0)
	{
		return -1; // Unknown expiry
	}

	const int64 Now = GetCurrentUnixTime();
	const int64 TimeRemaining = TokenExpiresAt - Now;
	
	return FMath::Max<int64>(0, TimeRemaining);
}

void FO3DTokenManager::Reset()
{
	FScopeLock TokenLock(&TokenMutex);
	FScopeLock CallbackLock(&CallbackMutex);
	
	CurrentToken.Empty();
	TokenExpiresAt = 0;
	bTokenValid = false;
	bRefreshInProgress = false;
	PendingCallbacks.Empty();
	
	if (TokenFetcher.IsValid())
	{
		TokenFetcher.Reset();
	}
}

int64 FO3DTokenManager::ParseJwtExpiry(const FString& JwtToken) const
{
	if (JwtToken.IsEmpty())
	{
		return 0;
	}

	// JWT format: header.payload.signature
	// We need to decode the payload (second part)
	TArray<FString> Parts;
	JwtToken.ParseIntoArray(Parts, TEXT("."));
	
	if (Parts.Num() != 3)
	{
		UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Invalid JWT format (expected 3 parts, got %d)"), Parts.Num());
		return 0;
	}

	// Base64 decode the payload
	FString PayloadBase64 = Parts[1];
	
	// JWT uses URL-safe Base64 encoding, need to convert to standard Base64
	PayloadBase64.ReplaceInline(TEXT("-"), TEXT("+"));
	PayloadBase64.ReplaceInline(TEXT("_"), TEXT("/"));
	
	// Add padding if needed
	while (PayloadBase64.Len() % 4 != 0)
	{
		PayloadBase64.AppendChar('=');
	}

	TArray<uint8> DecodedPayload;
	if (!FBase64::Decode(PayloadBase64, DecodedPayload))
	{
		UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Failed to Base64 decode JWT payload"));
		return 0;
	}

	// Convert to string
	FString PayloadJson;
	const int32 Utf8Length = DecodedPayload.Num();
	if (Utf8Length > 0)
	{
		// Convert UTF-8 to TCHAR
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(DecodedPayload.GetData()), Utf8Length);
		PayloadJson = FString(Converter.Length(), Converter.Get());
	}

	// Parse JSON to extract "exp" claim
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(PayloadJson);
	
	if (!FJsonSerializer::Deserialize(JsonReader, JsonObject) || !JsonObject.IsValid())
	{
		UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Failed to parse JWT payload JSON"));
		return 0;
	}

	// Extract "exp" claim (Unix timestamp in seconds)
	int64 ExpiryTimestamp = 0;
	if (JsonObject->TryGetNumberField(TEXT("exp"), ExpiryTimestamp))
	{
		UE_LOG(LogO3DWebRTCTokenManager, Verbose, TEXT("JWT expiry parsed: %lld (Unix timestamp)"), ExpiryTimestamp);
		return ExpiryTimestamp;
	}

	UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("JWT payload missing 'exp' claim"));
	return 0;
}

int64 FO3DTokenManager::GetCurrentUnixTime() const
{
	// FPlatformTime::Seconds() returns seconds since process start
	// We need Unix timestamp, so we use system time
	FDateTime Now = FDateTime::UtcNow();
	return Now.ToUnixTimestamp();
}

void FO3DTokenManager::OnTokenFetchComplete(const FO3DTokenResult& Result)
{
	// Update token if successful
	if (Result.bSuccess && !Result.Token.IsEmpty())
	{
		FScopeLock Lock(&TokenMutex);
		
		CurrentToken = Result.Token;
		bTokenValid = true;
		
		// Use provided expiry or parse from JWT
		if (Result.ExpiresAt > 0)
		{
			TokenExpiresAt = Result.ExpiresAt;
		}
		else
		{
			TokenExpiresAt = ParseJwtExpiry(CurrentToken);
		}

		const int64 TimeUntilExpiry = TokenExpiresAt > 0 ? (TokenExpiresAt - GetCurrentUnixTime()) : -1;
		
		UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token fetch completed successfully (expires in %lld seconds)"), TimeUntilExpiry);
	}
	else
	{
		UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Token fetch failed: %s"), *Result.ErrorMessage);
	}

	// Notify all pending callbacks
	TArray<TFunction<void(const FO3DTokenResult&)>> CallbacksCopy;
	{
		FScopeLock Lock(&CallbackMutex);
		CallbacksCopy = PendingCallbacks;
		PendingCallbacks.Empty();
		bRefreshInProgress = false;
	}

	for (const auto& Callback : CallbacksCopy)
	{
		if (Callback)
		{
			Callback(Result);
		}
	}
}
