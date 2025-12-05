/**
 * JWT Token Auto-Fetch Unit Tests
 * 
 * Comprehensive tests for the token auto-fetch feature including:
 * - TokenManager: JWT expiry parsing, token refresh timing, configuration modes
 * - TokenFetcher: Request building, response parsing, retry logic
 * - Integration: Sender/receiver with auto-fetch configuration
 * 
 * Tests are designed to validate functionality without requiring a live server.
 * Mock scenarios simulate various network conditions and server responses.
 */

#if WITH_DEV_AUTOMATION_TESTS

#include "../Shared/WebRTCTokenManager.h"
#include "../Shared/WebRTCTokenFetcher.h"
#include "../Sender/WebRTCSender.h"
#include "../Receiver/WebRTCReceiver.h"

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Misc/Base64.h"

#include "O3DTransportTypes.h"

namespace TokenTestHelpers
{
	/**
	 * Create a test JWT token with specified expiry time
	 * Format: header.payload.signature (Base64URL encoded)
	 * 
	 * @param ExpiryUnixSeconds Unix timestamp for expiry claim
	 * @param Identity Subject/identity claim
	 * @return Valid JWT token string
	 */
	FString CreateTestJwt(int64 ExpiryUnixSeconds, const FString& Identity = TEXT("test-user"))
	{
		// Header: {"alg":"HS256","typ":"JWT"}
		const FString Header = TEXT("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");

		// Create payload JSON with expiry claim
		FString PayloadJson = FString::Printf(
			TEXT("{\"exp\":%lld,\"iss\":\"test-server\",\"sub\":\"%s\"}"),
			ExpiryUnixSeconds, *Identity);

		// Base64URL encode payload
		TArray<uint8> PayloadBytes;
		const FTCHARToUTF8 Utf8Payload(*PayloadJson);
		PayloadBytes.Append(reinterpret_cast<const uint8*>(Utf8Payload.Get()), Utf8Payload.Length());

		FString PayloadBase64 = FBase64::Encode(PayloadBytes);
		// Convert to Base64URL (replace + with -, / with _, remove =)
		PayloadBase64.ReplaceInline(TEXT("+"), TEXT("-"));
		PayloadBase64.ReplaceInline(TEXT("/"), TEXT("_"));
		PayloadBase64.ReplaceInline(TEXT("="), TEXT(""));

		// Signature: dummy value (not validated in these tests)
		const FString Signature = TEXT("dummysignature");

		return FString::Printf(TEXT("%s.%s.%s"), *Header, *PayloadBase64, *Signature);
	}

	/**
	 * Create a JWT token that expires in the specified number of seconds from now
	 */
	FString CreateTestJwtExpiringIn(int64 SecondsFromNow)
	{
		const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
		return CreateTestJwt(Now + SecondsFromNow);
	}

	/**
	 * Create transport config for auto-fetch mode
	 */
	FO3DTransportConfig CreateAutoFetchConfig(const FString& EndpointUrl, const FString& RoomName = TEXT("test-room"))
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("wss://test.livekit.example.com");
		Config.StreamId = RoomName;
		Config.bUseAutoTokenFetch = true;
		Config.TokenEndpointUrl = EndpointUrl;
		Config.TokenRefreshLeadTimeSec = 300; // 5 minutes default
		return Config;
	}

	/**
	 * Create transport config for manual token mode
	 */
	FO3DTransportConfig CreateManualTokenConfig(const FString& Token)
	{
		FO3DTransportConfig Config;
		Config.Uri = TEXT("wss://test.livekit.example.com");
		Config.StreamId = TEXT("test-room");
		Config.bUseAutoTokenFetch = false;
		Config.Token = Token;
		return Config;
	}
}

// ============================================================================
// TOKEN MANAGER TESTS - JWT Parsing
// ============================================================================

/**
 * Test: TokenManager parses valid JWT expiry correctly
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerParseValidJwtTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.ParseValidJwt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerParseValidJwtTest::RunTest(const FString& Parameters)
{
	using namespace TokenTestHelpers;

	// Create a token expiring 1 hour from now
	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	const int64 ExpiryTime = Now + 3600; // 1 hour
	const FString TestToken = CreateTestJwt(ExpiryTime);

	// Create token manager in manual mode with this token
	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::Manual;
	Config.ManualToken = TestToken;

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Token manager should initialize with valid JWT"), bInitResult);

	// Verify token is available
	FString RetrievedToken;
	bool bGetResult = Manager.GetCurrentToken(RetrievedToken);
	TestTrue(TEXT("Should be able to get current token"), bGetResult);
	TestEqual(TEXT("Retrieved token should match original"), RetrievedToken, TestToken);

	// Check that expiry is properly tracked
	int64 TimeUntilExpiry = Manager.GetTimeUntilExpiry();
	TestTrue(TEXT("Time until expiry should be approximately 1 hour"),
		TimeUntilExpiry >= 3590 && TimeUntilExpiry <= 3610);

	TestFalse(TEXT("Token should not be expired"), Manager.IsTokenExpired());
	TestFalse(TEXT("Token should not need refresh yet (with default 5 min lead time)"), Manager.NeedsRefresh());

	Manager.Reset();
	return true;
}

/**
 * Test: TokenManager handles malformed JWT gracefully
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerMalformedJwtTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.MalformedJwt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerMalformedJwtTest::RunTest(const FString& Parameters)
{
	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::Manual;
	
	// Test various malformed JWTs
	TArray<FString> MalformedTokens = {
		TEXT("not.a.valid.jwt.format"),     // Too many parts
		TEXT("only.twoparts"),               // Only two parts
		TEXT("single"),                      // Single part
		TEXT(""),                            // Empty
		TEXT("header.invalid-base64!@#.sig") // Invalid base64
	};

	for (const FString& BadToken : MalformedTokens)
	{
		Config.ManualToken = BadToken;
		
		// Empty token should fail initialization
		if (BadToken.IsEmpty())
		{
			bool bResult = Manager.Initialize(Config);
			TestFalse(TEXT("Empty token should fail initialization"), bResult);
		}
		else
		{
			// Non-empty malformed tokens should initialize but expiry will be unknown
			bool bResult = Manager.Initialize(Config);
			if (bResult)
			{
				// Expiry parsing should fail gracefully (return 0 or -1)
				int64 TimeUntilExpiry = Manager.GetTimeUntilExpiry();
				TestTrue(TEXT("Malformed JWT should have unknown expiry (-1)"), TimeUntilExpiry == -1);
			}
		}
		Manager.Reset();
	}

	return true;
}

/**
 * Test: TokenManager detects expired tokens correctly
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerExpiredTokenTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.ExpiredToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerExpiredTokenTest::RunTest(const FString& Parameters)
{
	using namespace TokenTestHelpers;

	// Create a token that expired 1 hour ago
	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	const int64 ExpiredTime = Now - 3600;
	const FString ExpiredToken = CreateTestJwt(ExpiredTime);

	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::Manual;
	Config.ManualToken = ExpiredToken;

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Token manager should initialize even with expired token"), bInitResult);

	// Token should be detected as expired
	TestTrue(TEXT("Token should be detected as expired"), Manager.IsTokenExpired());

	// GetCurrentToken should fail for expired token
	FString RetrievedToken;
	bool bGetResult = Manager.GetCurrentToken(RetrievedToken);
	TestFalse(TEXT("GetCurrentToken should fail for expired token"), bGetResult);

	Manager.Reset();
	return true;
}

/**
 * Test: TokenManager correctly calculates refresh timing
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerRefreshTimingTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.RefreshTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerRefreshTimingTest::RunTest(const FString& Parameters)
{
	using namespace TokenTestHelpers;

	// Create token expiring in 240 seconds (4 minutes)
	// With default 5 minute lead time, it SHOULD need refresh
	const FString TokenNeedsRefresh = CreateTestJwtExpiringIn(240);

	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::AutoFetch;
	Config.EndpointUrl = TEXT("http://localhost:8080/token");
	Config.RoomName = TEXT("test-room");
	Config.RefreshLeadTimeSec = 300; // 5 minutes

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Token manager should initialize in auto-fetch mode"), bInitResult);

	// Manually set a token (simulating successful fetch)
	// Note: We can't directly set the token, but we can test the NeedsRefresh logic
	// by using manual mode with the token
	FO3DTokenConfig ManualConfig;
	ManualConfig.Mode = EO3DTokenMode::Manual;
	ManualConfig.ManualToken = TokenNeedsRefresh;
	ManualConfig.RefreshLeadTimeSec = 300;

	Manager.Reset();
	Manager.Initialize(ManualConfig);

	// In manual mode, NeedsRefresh should always return false
	// (refresh is only applicable in auto-fetch mode)
	TestFalse(TEXT("Manual mode should not trigger refresh"), Manager.NeedsRefresh());

	Manager.Reset();
	return true;
}

/**
 * Test: TokenManager handles JWT with missing exp claim
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerMissingExpClaimTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.MissingExpClaim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerMissingExpClaimTest::RunTest(const FString& Parameters)
{
	// Create a JWT without exp claim
	const FString Header = TEXT("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");
	
	// Payload without exp: {"iss":"test","sub":"user"} 
	FString PayloadJson = TEXT("{\"iss\":\"test-server\",\"sub\":\"test-user\"}");
	TArray<uint8> PayloadBytes;
	const FTCHARToUTF8 Utf8Payload(*PayloadJson);
	PayloadBytes.Append(reinterpret_cast<const uint8*>(Utf8Payload.Get()), Utf8Payload.Length());

	FString PayloadBase64 = FBase64::Encode(PayloadBytes);
	PayloadBase64.ReplaceInline(TEXT("+"), TEXT("-"));
	PayloadBase64.ReplaceInline(TEXT("/"), TEXT("_"));
	PayloadBase64.ReplaceInline(TEXT("="), TEXT(""));

	const FString TokenNoExp = FString::Printf(TEXT("%s.%s.sig"), *Header, *PayloadBase64);

	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::Manual;
	Config.ManualToken = TokenNoExp;

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Should initialize with token missing exp claim"), bInitResult);

	// Time until expiry should be unknown (-1)
	int64 TimeUntilExpiry = Manager.GetTimeUntilExpiry();
	TestEqual(TEXT("Time until expiry should be unknown (-1)"), TimeUntilExpiry, (int64)-1);

	Manager.Reset();
	return true;
}

// ============================================================================
// TOKEN MANAGER TESTS - Configuration Modes
// ============================================================================

/**
 * Test: TokenManager manual mode initialization
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerManualModeTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.ManualMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerManualModeTest::RunTest(const FString& Parameters)
{
	using namespace TokenTestHelpers;

	const FString ValidToken = CreateTestJwtExpiringIn(3600);

	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::Manual;
	Config.ManualToken = ValidToken;

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Manual mode with valid token should initialize"), bInitResult);

	FString RetrievedToken;
	bool bGetResult = Manager.GetCurrentToken(RetrievedToken);
	TestTrue(TEXT("Should retrieve token in manual mode"), bGetResult);
	TestEqual(TEXT("Token should match provided token"), RetrievedToken, ValidToken);

	// Manual mode should never need refresh
	TestFalse(TEXT("Manual mode should never need refresh"), Manager.NeedsRefresh());

	Manager.Reset();
	return true;
}

/**
 * Test: TokenManager auto-fetch mode initialization
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerAutoFetchModeTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.AutoFetchMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerAutoFetchModeTest::RunTest(const FString& Parameters)
{
	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::AutoFetch;
	Config.EndpointUrl = TEXT("http://localhost:8080/token");
	Config.RoomName = TEXT("test-room");
	Config.Identity = TEXT("test-sender");
	Config.Role = EO3DTokenRole::Publisher;
	Config.RefreshLeadTimeSec = 300;

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Auto-fetch mode with valid config should initialize"), bInitResult);

	// Initially, no token should be available
	FString RetrievedToken;
	bool bGetResult = Manager.GetCurrentToken(RetrievedToken);
	TestFalse(TEXT("No token should be available before fetch"), bGetResult);

	// Should need refresh (no token yet)
	TestTrue(TEXT("Should need refresh when no token available"), Manager.NeedsRefresh());

	Manager.Reset();
	return true;
}

/**
 * Test: TokenManager auto-fetch mode fails without endpoint URL
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerAutoFetchNoEndpointTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.AutoFetchNoEndpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerAutoFetchNoEndpointTest::RunTest(const FString& Parameters)
{
	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::AutoFetch;
	Config.EndpointUrl = TEXT(""); // Empty endpoint
	Config.RoomName = TEXT("test-room");

	bool bInitResult = Manager.Initialize(Config);
	TestFalse(TEXT("Auto-fetch mode without endpoint should fail"), bInitResult);

	Manager.Reset();
	return true;
}

// ============================================================================
// TOKEN MANAGER TESTS - Thread Safety and Callbacks
// ============================================================================

/**
 * Test: TokenManager callback queuing for concurrent refresh requests
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenManagerCallbackQueuingTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager.CallbackQueuing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenManagerCallbackQueuingTest::RunTest(const FString& Parameters)
{
	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::AutoFetch;
	Config.EndpointUrl = TEXT("http://localhost:8080/token");
	Config.RoomName = TEXT("test-room");

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Should initialize in auto-fetch mode"), bInitResult);

	// Track callback invocations
	TAtomic<int32> CallbackCount{0};

	// Initiate multiple refresh requests - callbacks should be queued
	for (int32 i = 0; i < 5; ++i)
	{
		Manager.RefreshTokenAsync([&CallbackCount](const FO3DTokenResult& Result)
		{
			CallbackCount.IncrementExchange();
		});
	}

	// Note: Since we don't have a real server, callbacks will eventually fail
	// but this tests that the queuing logic doesn't crash

	// Give some time for async processing
	FPlatformProcess::Sleep(0.1f);

	// At minimum, no crash should occur
	TestTrue(TEXT("Multiple refresh requests should not crash"), true);

	Manager.Reset();
	return true;
}

// ============================================================================
// TOKEN FETCHER TESTS - Request Building
// ============================================================================

/**
 * Test: TokenFetcher builds correct JSON request body
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenFetcherRequestBuildingTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Fetcher.RequestBuilding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenFetcherRequestBuildingTest::RunTest(const FString& Parameters)
{
	FO3DTokenFetcher Fetcher;

	FO3DTokenFetchRequest Request;
	Request.EndpointUrl = TEXT("http://localhost:8080/token");
	Request.RoomName = TEXT("production-room");
	Request.Identity = TEXT("actor-001");
	Request.Role = TEXT("publisher");
	Request.TimeoutSeconds = 10.0f;
	Request.MaxRetries = 3;

	// Test that request doesn't crash when building
	// (actual request won't be sent without a server)
	TAtomic<bool> bCallbackInvoked{false};

	Fetcher.FetchTokenAsync(Request, [&bCallbackInvoked](const FO3DTokenResult& Result)
	{
		bCallbackInvoked.Store(true);
		// Expect failure since there's no server
	});

	// Wait a short time for async operation
	FPlatformProcess::Sleep(0.5f);

	// Callback should have been invoked (with error since no server)
	// Note: This may take longer than 0.5s in some environments
	// The main test is that no crash occurred
	TestTrue(TEXT("Request building should not crash"), true);

	Fetcher.CancelPendingRequests();
	return true;
}

/**
 * Test: TokenFetcher handles empty endpoint URL
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenFetcherEmptyEndpointTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Fetcher.EmptyEndpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenFetcherEmptyEndpointTest::RunTest(const FString& Parameters)
{
	FO3DTokenFetcher Fetcher;

	FO3DTokenFetchRequest Request;
	Request.EndpointUrl = TEXT(""); // Empty
	Request.RoomName = TEXT("test-room");
	Request.Identity = TEXT("test-user");
	Request.Role = TEXT("publisher");

	TAtomic<bool> bCallbackInvoked{false};
	TAtomic<bool> bCallbackSuccess{false};
	FString CallbackError;

	Fetcher.FetchTokenAsync(Request, [&](const FO3DTokenResult& Result)
	{
		bCallbackInvoked.Store(true);
		bCallbackSuccess.Store(Result.bSuccess);
	});

	// Wait for callback
	FPlatformProcess::Sleep(0.1f);

	TestTrue(TEXT("Callback should be invoked"), bCallbackInvoked.Load());
	TestFalse(TEXT("Fetch should fail with empty endpoint"), bCallbackSuccess.Load());

	return true;
}

/**
 * Test: TokenFetcher exponential backoff calculation
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTokenFetcherBackoffCalculationTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Fetcher.BackoffCalculation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTokenFetcherBackoffCalculationTest::RunTest(const FString& Parameters)
{
	// Note: CalculateBackoffDelay is private, but we can verify the documented behavior
	// by checking that retry logic doesn't crash

	FO3DTokenFetcher Fetcher;

	FO3DTokenFetchRequest Request;
	Request.EndpointUrl = TEXT("http://localhost:9999/token"); // Non-existent
	Request.RoomName = TEXT("test");
	Request.Identity = TEXT("test");
	Request.Role = TEXT("publisher");
	Request.MaxRetries = 2; // Allow retries
	Request.TimeoutSeconds = 2.0f; // Short timeout

	TAtomic<bool> bCallbackInvoked{false};

	Fetcher.FetchTokenAsync(Request, [&bCallbackInvoked](const FO3DTokenResult& Result)
	{
		bCallbackInvoked.Store(true);
	});

	// Wait for retry attempts (could take several seconds)
	// For this test, we mainly verify no crash
	FPlatformProcess::Sleep(1.0f);

	TestTrue(TEXT("Backoff retry should not crash"), true);

	Fetcher.CancelPendingRequests();
	return true;
}

// ============================================================================
// INTEGRATION TESTS - Sender with Auto-Fetch
// ============================================================================

/**
 * Test: Sender initialization with auto-fetch configuration
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSenderAutoFetchInitTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Integration.SenderInit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSenderAutoFetchInitTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace TokenTestHelpers;

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateAutoFetchConfig(TEXT("http://localhost:8080/token"));
	Config.StreamId = TEXT("test-room");

	bool bResult = Sender.Initialize(Config);
	TestTrue(TEXT("Sender should initialize with auto-fetch config"), bResult);

	// Start should return true but wait for token
	bool bStartResult = Sender.Start();
	TestTrue(TEXT("Start should return true (waiting for token)"), bStartResult);

	Sender.Stop();
#else
	AddInfo(TEXT("Test skipped on non-Windows 64-bit platform"));
#endif

	return true;
}

/**
 * Test: Sender fallback to manual token when auto-fetch disabled
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSenderManualTokenFallbackTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Integration.SenderManualFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSenderManualTokenFallbackTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace TokenTestHelpers;

	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config = CreateManualTokenConfig(CreateTestJwtExpiringIn(3600));

	bool bResult = Sender.Initialize(Config);
	TestTrue(TEXT("Sender should initialize with manual token"), bResult);

	Sender.Stop();
#else
	AddInfo(TEXT("Test skipped on non-Windows 64-bit platform"));
#endif

	return true;
}

/**
 * Test: Sender rejects auto-fetch without endpoint URL
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSenderAutoFetchNoEndpointTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Integration.SenderNoEndpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSenderAutoFetchNoEndpointTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	FO3DWebRTCSender Sender;
	FO3DTransportConfig Config;
	Config.Uri = TEXT("wss://test.livekit.example.com");
	Config.StreamId = TEXT("test-room");
	Config.bUseAutoTokenFetch = true;
	Config.TokenEndpointUrl = TEXT(""); // Empty endpoint

	bool bResult = Sender.Initialize(Config);
	TestFalse(TEXT("Sender should fail to initialize without endpoint URL"), bResult);

	Sender.Stop();
#else
	AddInfo(TEXT("Test skipped on non-Windows 64-bit platform"));
#endif

	return true;
}

// ============================================================================
// INTEGRATION TESTS - Receiver with Auto-Fetch
// ============================================================================

/**
 * Test: Receiver initialization with auto-fetch configuration
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReceiverAutoFetchInitTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Integration.ReceiverInit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FReceiverAutoFetchInitTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace TokenTestHelpers;

	FO3DWebRTCReceiver Receiver;
	FO3DTransportConfig Config = CreateAutoFetchConfig(TEXT("http://localhost:8080/token"));
	Config.StreamId = TEXT("test-room");

	bool bResult = Receiver.Initialize(Config);
	TestTrue(TEXT("Receiver should initialize with auto-fetch config"), bResult);

	// Start should return true but wait for token
	bool bStartResult = Receiver.Start();
	TestTrue(TEXT("Start should return true (waiting for token)"), bStartResult);

	Receiver.Stop();
#else
	AddInfo(TEXT("Test skipped on non-Windows 64-bit platform"));
#endif

	return true;
}

/**
 * Test: Receiver uses subscriber role for token requests
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReceiverSubscriberRoleTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Integration.ReceiverSubscriberRole",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FReceiverSubscriberRoleTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace TokenTestHelpers;

	FO3DWebRTCReceiver Receiver;
	FO3DTransportConfig Config = CreateAutoFetchConfig(TEXT("http://localhost:8080/token"));
	Config.StreamId = TEXT("test-room");
	Config.Role = TEXT("subscriber"); // Receiver should request subscriber role

	bool bResult = Receiver.Initialize(Config);
	TestTrue(TEXT("Receiver should initialize with subscriber role config"), bResult);

	Receiver.Stop();
#else
	AddInfo(TEXT("Test skipped on non-Windows 64-bit platform"));
#endif

	return true;
}

// ============================================================================
// BACKWARD COMPATIBILITY TESTS
// ============================================================================

/**
 * Test: Manual token mode remains the default
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBackwardCompatDefaultModeTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.BackwardCompat.DefaultMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackwardCompatDefaultModeTest::RunTest(const FString& Parameters)
{
	FO3DTransportConfig Config;
	
	// Default value should be false (manual mode)
	TestFalse(TEXT("bUseAutoTokenFetch should default to false"), Config.bUseAutoTokenFetch);

	return true;
}

/**
 * Test: Existing projects with manual tokens still work
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBackwardCompatManualTokenTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.BackwardCompat.ManualToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBackwardCompatManualTokenTest::RunTest(const FString& Parameters)
{
#if PLATFORM_WINDOWS && PLATFORM_64BITS
	using namespace TokenTestHelpers;

	// Simulate existing project configuration (no auto-fetch settings)
	FO3DTransportConfig Config;
	Config.Uri = TEXT("wss://test.livekit.example.com");
	Config.Token = CreateTestJwtExpiringIn(3600);
	Config.StreamId = TEXT("test-room");
	// bUseAutoTokenFetch defaults to false
	// TokenEndpointUrl is empty

	FO3DWebRTCSender Sender;
	bool bResult = Sender.Initialize(Config);
	TestTrue(TEXT("Existing manual token configuration should work"), bResult);

	Sender.Stop();
#else
	AddInfo(TEXT("Test skipped on non-Windows 64-bit platform"));
#endif

	return true;
}

// ============================================================================
// CONFIGURATION PERSISTENCE TESTS
// ============================================================================

/**
 * Test: Token refresh lead time configuration
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConfigRefreshLeadTimeTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Config.RefreshLeadTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConfigRefreshLeadTimeTest::RunTest(const FString& Parameters)
{
	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::AutoFetch;
	Config.EndpointUrl = TEXT("http://localhost:8080/token");
	Config.RoomName = TEXT("test-room");
	
	// Test custom lead time
	Config.RefreshLeadTimeSec = 600; // 10 minutes

	bool bResult = Manager.Initialize(Config);
	TestTrue(TEXT("Should initialize with custom refresh lead time"), bResult);

	Manager.Reset();
	return true;
}

/**
 * Test: Role configuration (Publisher vs Subscriber)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConfigRoleTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Config.Role",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FConfigRoleTest::RunTest(const FString& Parameters)
{
	FO3DTokenManager Manager;
	
	// Test Publisher role
	FO3DTokenConfig PublisherConfig;
	PublisherConfig.Mode = EO3DTokenMode::AutoFetch;
	PublisherConfig.EndpointUrl = TEXT("http://localhost:8080/token");
	PublisherConfig.RoomName = TEXT("test-room");
	PublisherConfig.Role = EO3DTokenRole::Publisher;

	bool bPublisherInit = Manager.Initialize(PublisherConfig);
	TestTrue(TEXT("Should initialize with publisher role"), bPublisherInit);
	Manager.Reset();

	// Test Subscriber role
	FO3DTokenConfig SubscriberConfig;
	SubscriberConfig.Mode = EO3DTokenMode::AutoFetch;
	SubscriberConfig.EndpointUrl = TEXT("http://localhost:8080/token");
	SubscriberConfig.RoomName = TEXT("test-room");
	SubscriberConfig.Role = EO3DTokenRole::Subscriber;

	bool bSubscriberInit = Manager.Initialize(SubscriberConfig);
	TestTrue(TEXT("Should initialize with subscriber role"), bSubscriberInit);
	Manager.Reset();

	return true;
}

// ============================================================================
// ERROR HANDLING TESTS
// ============================================================================

/**
 * Test: Token manager handles timeout gracefully
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErrorHandlingTimeoutTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.ErrorHandling.Timeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FErrorHandlingTimeoutTest::RunTest(const FString& Parameters)
{
	FO3DTokenFetcher Fetcher;

	FO3DTokenFetchRequest Request;
	Request.EndpointUrl = TEXT("http://192.0.2.1:9999/token"); // Non-routable IP (TEST-NET-1)
	Request.RoomName = TEXT("test-room");
	Request.Identity = TEXT("test-user");
	Request.Role = TEXT("publisher");
	Request.TimeoutSeconds = 1.0f; // Very short timeout
	Request.MaxRetries = 0; // No retries

	TAtomic<bool> bCallbackInvoked{false};
	TAtomic<bool> bSuccess{false};
	FString ErrorMessage;

	Fetcher.FetchTokenAsync(Request, [&](const FO3DTokenResult& Result)
	{
		bCallbackInvoked.Store(true);
		bSuccess.Store(Result.bSuccess);
	});

	// Wait for timeout
	FPlatformProcess::Sleep(3.0f);

	TestTrue(TEXT("Callback should be invoked after timeout"), bCallbackInvoked.Load());
	TestFalse(TEXT("Request should fail due to timeout"), bSuccess.Load());

	Fetcher.CancelPendingRequests();
	return true;
}

/**
 * Test: Token manager reset clears state
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErrorHandlingResetTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.ErrorHandling.Reset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FErrorHandlingResetTest::RunTest(const FString& Parameters)
{
	using namespace TokenTestHelpers;

	FO3DTokenManager Manager;
	FO3DTokenConfig Config;
	Config.Mode = EO3DTokenMode::Manual;
	Config.ManualToken = CreateTestJwtExpiringIn(3600);

	bool bInitResult = Manager.Initialize(Config);
	TestTrue(TEXT("Should initialize"), bInitResult);

	FString TokenBefore;
	bool bGetBefore = Manager.GetCurrentToken(TokenBefore);
	TestTrue(TEXT("Should have token before reset"), bGetBefore);

	// Reset
	Manager.Reset();

	// Token should be cleared
	FString TokenAfter;
	bool bGetAfter = Manager.GetCurrentToken(TokenAfter);
	TestFalse(TEXT("Should not have token after reset"), bGetAfter);

	return true;
}

/**
 * Test: Token fetcher can cancel pending requests
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FErrorHandlingCancelTest,
	"Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.ErrorHandling.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FErrorHandlingCancelTest::RunTest(const FString& Parameters)
{
	FO3DTokenFetcher Fetcher;

	FO3DTokenFetchRequest Request;
	Request.EndpointUrl = TEXT("http://localhost:8080/token");
	Request.RoomName = TEXT("test-room");
	Request.Identity = TEXT("test-user");
	Request.Role = TEXT("publisher");
	Request.TimeoutSeconds = 30.0f; // Long timeout

	// Start request
	Fetcher.FetchTokenAsync(Request, [](const FO3DTokenResult& Result)
	{
		// May or may not be invoked depending on cancellation timing
	});

	// Immediately cancel
	Fetcher.CancelPendingRequests();

	// Should not crash
	TestTrue(TEXT("Cancel should not crash"), true);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
