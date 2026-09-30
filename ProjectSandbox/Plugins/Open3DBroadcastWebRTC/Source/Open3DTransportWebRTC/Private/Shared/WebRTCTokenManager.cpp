// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_WEBRTC // Whole file: without the transport the module is a stub (O3DWebRtcBuildFlags).

#include "WebRTCTokenManager.h"
#include "WebRTCTokenFetcher.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "O3DHelpers.h"
#include "O3DRedact.h"

DEFINE_LOG_CATEGORY(LogO3DWebRTCTokenManager);

FO3DTokenManager::FO3DTokenManager(FO3DTokenFetcherFactory InFetcherFactory)
	: FetcherFactory(MoveTemp(InFetcherFactory))
	, State(MakeShared<FState, ESPMode::ThreadSafe>())
{
}

FO3DTokenManager::~FO3DTokenManager()
{
	Reset();
}

bool FO3DTokenManager::Initialize(const FO3DTokenConfig& InConfig)
{
	// Drop any fetch from a previous configuration first.
	Reset();

	FScopeLock Lock(&State->Mutex);

	State->Config = InConfig;
	State->bTokenValid = false;
	State->CurrentToken.Empty();
	State->TokenExpiresAt = 0;
	State->bExpiryLogged = false;

	if (InConfig.Mode == EO3DTokenMode::Manual)
	{
		// Manual mode: use provided token directly
		if (InConfig.ManualToken.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Manual token mode but no token provided"));
			return false;
		}

		State->CurrentToken = InConfig.ManualToken;
		State->TokenExpiresAt = ParseJwtExpiry(State->CurrentToken);
		State->bTokenValid = true;
		++State->TokenGeneration;

		UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token manager initialized (Manual mode)"));
		return true;
	}
	else if (InConfig.Mode == EO3DTokenMode::AutoFetch)
	{
		// Auto-fetch mode: validate configuration and create fetcher
		if (InConfig.EndpointUrl.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Auto-fetch mode but no endpoint URL provided"));
			return false;
		}

		// Credentials never travel over plain HTTP except to this machine (ADR 0004 item 6).
		if (!O3DHelpers::IsHttpsOrLoopbackHttpUrl(InConfig.EndpointUrl))
		{
			UE_LOG(LogO3DWebRTCTokenManager, Error,
				TEXT("Token endpoint %s refused: use https:// (plain http:// is allowed only for localhost, 127.0.0.1 and ::1)"),
				*O3DRedact::Url(InConfig.EndpointUrl));
			return false;
		}

		if (InConfig.RoomName.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Auto-fetch mode but no room name provided"));
			return false;
		}

		if (InConfig.Identity.IsEmpty())
		{
			UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Auto-fetch mode but no identity provided"));
		}

		if (FetcherFactory)
		{
			TokenFetcher = FetcherFactory();
		}
		else
		{
			TokenFetcher = MakeShared<FO3DTokenFetcher, ESPMode::ThreadSafe>();
		}

		UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token manager initialized (Auto-fetch mode, endpoint: %s, endpoint auth: %s)"),
			*O3DRedact::Url(InConfig.EndpointUrl), InConfig.EndpointAuth.IsEmpty() ? TEXT("none") : TEXT("set"));
		return true;
	}

	return false;
}

bool FO3DTokenManager::GetCurrentToken(FString& OutToken, uint64* OutGeneration) const
{
	FScopeLock Lock(&State->Mutex);

	if (OutGeneration)
	{
		*OutGeneration = State->TokenGeneration;
	}

	if (!State->bTokenValid || State->CurrentToken.IsEmpty())
	{
		return false;
	}

	// Check if token is expired
	if (State->TokenExpiresAt > 0)
	{
		const int64 Now = GetCurrentUnixTime();
		if (Now >= State->TokenExpiresAt)
		{
			// Logged once per token; Tick/Poll call this every frame (TRF-23).
			if (!State->bExpiryLogged)
			{
				State->bExpiryLogged = true;
				UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("Current token is expired"));
			}
			return false;
		}
	}

	OutToken = State->CurrentToken;
	return true;
}

uint64 FO3DTokenManager::GetTokenGeneration() const
{
	FScopeLock Lock(&State->Mutex);
	return State->TokenGeneration;
}

bool FO3DTokenManager::IsRefreshInProgress() const
{
	FScopeLock Lock(&State->Mutex);
	return State->bRefreshInProgress;
}

bool FO3DTokenManager::IsAutoFetch() const
{
	FScopeLock Lock(&State->Mutex);
	return State->Config.Mode == EO3DTokenMode::AutoFetch;
}

bool FO3DTokenManager::IsTokenExpired() const
{
	FScopeLock Lock(&State->Mutex);

	if (!State->bTokenValid || State->TokenExpiresAt == 0)
	{
		return true; // No token or unknown expiry = treat as expired
	}

	const int64 Now = GetCurrentUnixTime();
	return Now >= State->TokenExpiresAt;
}

bool FO3DTokenManager::NeedsRefresh() const
{
	FScopeLock Lock(&State->Mutex);

	// Manual mode never needs refresh
	if (State->Config.Mode == EO3DTokenMode::Manual)
	{
		return false;
	}

	// If no token, definitely need to fetch
	if (!State->bTokenValid || State->CurrentToken.IsEmpty())
	{
		return true;
	}

	// If expiry unknown, don't refresh automatically
	if (State->TokenExpiresAt == 0)
	{
		return false;
	}

	// Check if within refresh lead time
	const int64 Now = GetCurrentUnixTime();
	const int64 TimeUntilExpiry = State->TokenExpiresAt - Now;

	return TimeUntilExpiry <= State->Config.RefreshLeadTimeSec;
}

void FO3DTokenManager::RefreshTokenAsync(TFunction<void(const FO3DTokenResult&)> OnComplete)
{
	FO3DTokenFetchRequest Request;
	uint64 FetchEpoch = 0;
	{
		FScopeLock Lock(&State->Mutex);

		if (OnComplete)
		{
			State->PendingCallbacks.Add(MoveTemp(OnComplete));
		}

		if (State->bRefreshInProgress)
		{
			UE_LOG(LogO3DWebRTCTokenManager, Verbose, TEXT("Token refresh already in progress"));
			return;
		}

		if (State->Config.Mode == EO3DTokenMode::AutoFetch && TokenFetcher.IsValid())
		{
			State->bRefreshInProgress = true;
			FetchEpoch = State->FetchEpoch;

			Request.EndpointUrl = State->Config.EndpointUrl;
			Request.RoomName = State->Config.RoomName;
			Request.Identity = State->Config.Identity;
			Request.Role = State->Config.Role == EO3DTokenRole::Publisher ? TEXT("publisher") : TEXT("subscriber");
			Request.AuthBearer = State->Config.EndpointAuth;
		}
	}

	if (Request.EndpointUrl.IsEmpty())
	{
		// Not in auto-fetch mode, or not initialized.
		UE_LOG(LogO3DWebRTCTokenManager, Warning, TEXT("RefreshTokenAsync called but auto-fetch is not configured"));
		FO3DTokenResult Result;
		Result.bSuccess = false;
		Result.ErrorMessage = TEXT("Token manager not in auto-fetch mode");
		uint64 CurrentEpoch = 0;
		{
			FScopeLock Lock(&State->Mutex);
			CurrentEpoch = State->FetchEpoch;
			State->bRefreshInProgress = true; // cleared by OnTokenFetchComplete
		}
		OnTokenFetchComplete(State, CurrentEpoch, Result);
		return;
	}

	UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Fetching token from endpoint: %s (room: %s, identity: %s, role: %s)"),
		*O3DRedact::Url(Request.EndpointUrl), *Request.RoomName, *Request.Identity, *Request.Role);

	// The callback holds the state weakly and never the manager (TRF-24).
	const TWeakPtr<FState, ESPMode::ThreadSafe> WeakState = State;
	TokenFetcher->FetchTokenAsync(Request, [WeakState, FetchEpoch](const FO3DTokenResult& Result)
	{
		if (const TSharedPtr<FState, ESPMode::ThreadSafe> Pinned = WeakState.Pin())
		{
			OnTokenFetchComplete(Pinned.ToSharedRef(), FetchEpoch, Result);
		}
	});
}

void FO3DTokenManager::CancelRefresh()
{
	if (TokenFetcher.IsValid())
	{
		TokenFetcher->CancelPendingRequests();
	}

	FScopeLock Lock(&State->Mutex);
	++State->FetchEpoch;
	State->bRefreshInProgress = false;
	State->PendingCallbacks.Empty();
}

int64 FO3DTokenManager::GetTimeUntilExpiry() const
{
	FScopeLock Lock(&State->Mutex);

	if (State->TokenExpiresAt == 0)
	{
		return -1; // Unknown expiry
	}

	const int64 Now = GetCurrentUnixTime();
	const int64 TimeRemaining = State->TokenExpiresAt - Now;

	return FMath::Max<int64>(0, TimeRemaining);
}

void FO3DTokenManager::Reset()
{
	// Cancel outside the state lock: a fetcher may complete synchronously on cancel.
	if (TokenFetcher.IsValid())
	{
		TokenFetcher->CancelPendingRequests();
		TokenFetcher.Reset();
	}

	FScopeLock Lock(&State->Mutex);
	State->CurrentToken.Empty();
	State->TokenExpiresAt = 0;
	State->bTokenValid = false;
	State->bExpiryLogged = false;
	State->bRefreshInProgress = false;
	++State->FetchEpoch;
	State->PendingCallbacks.Empty();
}

int64 FO3DTokenManager::ParseJwtExpiry(const FString& JwtToken)
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

int64 FO3DTokenManager::GetCurrentUnixTime()
{
	// FPlatformTime::Seconds() returns seconds since process start
	// We need Unix timestamp, so we use system time
	FDateTime Now = FDateTime::UtcNow();
	return Now.ToUnixTimestamp();
}

void FO3DTokenManager::OnTokenFetchComplete(const TSharedRef<FState, ESPMode::ThreadSafe>& InState, uint64 FetchEpoch, const FO3DTokenResult& Result)
{
	TArray<TFunction<void(const FO3DTokenResult&)>> CallbacksCopy;
	{
		FScopeLock Lock(&InState->Mutex);

		if (FetchEpoch != InState->FetchEpoch)
		{
			// Reset() or CancelRefresh() ran after this fetch started; drop the stale result.
			return;
		}

		if (Result.bSuccess && !Result.Token.IsEmpty())
		{
			InState->CurrentToken = Result.Token;
			InState->bTokenValid = true;
			InState->bExpiryLogged = false;
			++InState->TokenGeneration;

			// Use provided expiry or parse from JWT
			InState->TokenExpiresAt = Result.ExpiresAt > 0 ? Result.ExpiresAt : ParseJwtExpiry(InState->CurrentToken);

			const int64 TimeUntilExpiry = InState->TokenExpiresAt > 0 ? (InState->TokenExpiresAt - GetCurrentUnixTime()) : -1;
			UE_LOG(LogO3DWebRTCTokenManager, Log, TEXT("Token fetch completed successfully (expires in %lld seconds)"), TimeUntilExpiry);
		}
		else
		{
			UE_LOG(LogO3DWebRTCTokenManager, Error, TEXT("Token fetch failed: %s"), *Result.ErrorMessage);
		}

		CallbacksCopy = MoveTemp(InState->PendingCallbacks);
		InState->PendingCallbacks.Reset();
		InState->bRefreshInProgress = false;
	}

	for (const auto& Callback : CallbacksCopy)
	{
		if (Callback)
		{
			Callback(Result);
		}
	}
}

#endif // O3D_WITH_TRANSPORT_WEBRTC
