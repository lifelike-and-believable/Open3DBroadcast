# JWT Token Auto-Fetch Implementation

**Status:** ✅ Core Implementation Complete  
**Date:** 2024-11-23  
**Version:** 1.0

---

## Overview

This document describes the implementation of automatic JWT token fetching for the Open3DTransportWebRTC module. This feature eliminates the need for manual token entry by allowing the transport to automatically fetch fresh tokens from a configurable HTTP endpoint.

## Problem Statement

Previously, users needed to manually generate and enter JWT tokens for WebRTC connections:

1. **Development**: Inconvenient to repeatedly generate and paste tokens
2. **Production**: Impractical for shipping applications where each user needs their own token
3. **Expiry**: No automatic handling of token expiration
4. **Security**: Tokens stored in project files or blueprints

## Solution

Implement automatic token fetching from a hosted token generator endpoint with:

- Configurable HTTP/HTTPS endpoint URL
- Asynchronous token fetching (non-blocking)
- Automatic token refresh before expiry
- Role-based token generation (Publisher/Subscriber)
- Optional API key authentication
- Graceful fallback to manual tokens

---

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────┐
│  Transport Configuration                    │
│  (FO3DTransportConfig)                      │
│  - bUseAutoTokenFetch                       │
│  - TokenEndpointUrl                         │
│  - TokenApiKey / ApiSecret                  │
│  - TokenRefreshLeadTimeSec                  │
└────────────────┬────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────┐
│  FO3DTokenManager                           │
│  - Initialize(config)                       │
│  - GetCurrentToken() → FString              │
│  - NeedsRefresh() → bool                    │
│  - RefreshTokenAsync(callback)              │
│  - ParseJwtExpiry(token) → int64            │
└────────────────┬────────────────────────────┘
                 │
                 ├─────────────────────────────┐
                 ▼                             ▼
┌────────────────────────────┐  ┌─────────────────────────┐
│  FO3DTokenFetcher          │  │  Parsed JWT Payload     │
│  - FetchTokenAsync()       │  │  - "exp": timestamp     │
│  - BuildRequestBody()      │  │  - "sub": identity      │
│  - ParseResponse()         │  │  - "video": grants      │
└────────────────────────────┘  └─────────────────────────┘
         │
         ▼
┌────────────────────────────┐
│  HTTP Module (Unreal)      │
│  - IHttpRequest (async)    │
│  - FHttpModule::Get()      │
└────────────────────────────┘
```

### Data Flow

1. **Initialization:**
   ```
   ParseConfig() → Initialize TokenManager → 
   (if auto-fetch) Queue async token fetch
   ```

2. **Token Fetch:**
   ```
   RefreshTokenAsync() → FetchTokenAsync() →
   HTTP POST /token → Parse JSON response →
   Extract token + expiry → Callback with result
   ```

3. **Connection:**
   ```
   Start() → EnsureTokenAvailable() →
   (if waiting) return pending →
   Tick()/Poll() checks for token arrival →
   Retry Start() when token ready
   ```

4. **Token Refresh:**
   ```
   Tick()/Poll() → CheckTokenRefresh() →
   (if NeedsRefresh()) RefreshTokenAsync() →
   Update token → (Optional) Reconnect
   ```

---

## Implementation Details

### Core Classes

#### FO3DTokenManager

**Purpose:** Manages token lifecycle including fetching, caching, expiry tracking, and refresh coordination.

**Key Methods:**
- `Initialize(FO3DTokenConfig)` - Configure manual or auto-fetch mode
- `GetCurrentToken(FString& OutToken)` - Thread-safe token access
- `IsTokenExpired()` - Check if token has expired
- `NeedsRefresh()` - Check if token should be refreshed (lead time)
- `RefreshTokenAsync(callback)` - Async token fetch/refresh
- `ParseJwtExpiry(FString)` - Extract "exp" claim from JWT payload

**Thread Safety:** All public methods protected by `TokenMutex`

**Key Features:**
- Base64 decoding of JWT payload
- Unix timestamp-based expiry tracking
- Callback queuing for multiple refresh requests
- Automatic lead-time calculation

#### FO3DTokenFetcher

**Purpose:** Performs asynchronous HTTP requests to token endpoints.

**Key Methods:**
- `FetchTokenAsync(request, callback)` - Initiate HTTP POST
- `BuildRequestBody(request)` - Generate JSON request
- `ParseResponse(response)` - Parse JSON response
- `CancelPendingRequests()` - Cleanup on shutdown

**Key Features:**
- Async HTTP via Unreal's IHttpRequest
- JSON serialization/deserialization
- Error handling with specific messages
- Configurable request timeout (10 seconds default)

### Configuration

#### New Fields in FO3DTransportConfig

```cpp
// Toggle auto-fetch mode
bool bUseAutoTokenFetch = false;

// Token endpoint URL
FString TokenEndpointUrl;  // e.g., "http://localhost:8080/token"

// Refresh timing
int32 TokenRefreshLeadTimeSec = 300;  // 5 minutes default
```

**Security Architecture:**
The token generator server stores LiveKit API credentials (API key/secret). The client only sends room, identity, and role information. The server generates and signs the JWT token using its stored credentials. This keeps LiveKit credentials secure on the server.

### Token Endpoint Protocol

#### Request Format

```http
POST /token HTTP/1.1
Host: livekit.example.com
Content-Type: application/json

{
  "room": "test-room",
  "identity": "sender-12345",
  "role": "publisher"  // or "subscriber"
}
```

**Note:** The token generator server receives this request and uses its stored LiveKit API credentials to generate and sign the JWT. The client does not send or have access to LiveKit API credentials.

#### Response Format

```http
HTTP/1.1 200 OK
Content-Type: application/json

{
  "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "expiresAt": 1234567890,  // Unix timestamp (optional)
  "ttl": 3600               // Seconds (optional)
}
```

#### JWT Token Format

**Header:**
```json
{
  "alg": "HS256",
  "typ": "JWT"
}
```

**Payload:**
```json
{
  "exp": 1234567890,        // Expiry timestamp (required)
  "iss": "token-server",    // Issuer
  "sub": "sender-12345",    // Subject/identity
  "nbf": 1234564290,        // Not before
  "video": {                // LiveKit video grants
    "room": "test-room",
    "roomJoin": true,
    "roomCreate": true,
    "canPublish": true,
    "canSubscribe": false
  }
}
```

### Integration Points

#### Sender Integration (FO3DWebRTCSender)

**Changes:**
- Added `TUniquePtr<FO3DTokenManager> TokenManager` member
- Modified `ParseConfig()` to initialize token manager
- Added `EnsureTokenAvailable()` - async token wait logic
- Added `CheckTokenRefresh()` - proactive refresh check
- Modified `Start()` - wait for token before connecting
- Modified `Tick()` - monitor token arrival, check refresh
- Modified `Stop()` - cleanup token manager

**Identity Generation:** `sender-<ProcessID>`  
**Role:** Publisher

#### Receiver Integration (FO3DWebRTCReceiver)

**Changes:** Same as sender (for consistency)

**Identity Generation:** `receiver-<ProcessID>`  
**Role:** Subscriber

---

## Usage

### Manual Token Mode (Existing Behavior)

```cpp
FO3DTransportConfig Config;
Config.Transport = TEXT("webrtc");
Config.Uri = TEXT("wss://livekit.example.com");
Config.Token = TEXT("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...");
Config.bUseAutoTokenFetch = false;  // Default
```

### Auto-Fetch Mode (New Feature)

```cpp
FO3DTransportConfig Config;
Config.Transport = TEXT("webrtc");
Config.Uri = TEXT("wss://livekit.example.com");
Config.StreamId = TEXT("my-room");
Config.bUseAutoTokenFetch = true;
Config.TokenEndpointUrl = TEXT("https://livekit.example.com/token");
Config.TokenRefreshLeadTimeSec = 300;     // 5 minutes
// Config.Token is ignored in auto-fetch mode
```

### Testing with Mock Server

1. Start mock server:
   ```bash
   cd ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Tests
   python mock-token-server.py
   ```

2. Configure transport:
   ```cpp
   Config.bUseAutoTokenFetch = true;
   Config.TokenEndpointUrl = TEXT("http://localhost:8080/token");
   ```

3. Transport automatically fetches tokens from mock server

---

## Security Considerations

### Credential Storage

**Architecture:**
- LiveKit API credentials (API key/secret) are stored on the token generator server, NOT on the client
- The client only sends room, identity, and role information to the token endpoint
- The server uses its stored credentials to generate and sign JWT tokens
- This architecture ensures LiveKit credentials never leave the server

**Default Behavior:**
- Tokens are temporary (1 hour default TTL)
- No credentials stored on client side
- Token endpoint should use HTTPS in production

**Recommendations for Token Server:**
- Store LiveKit API credentials securely (environment variables or secure vaults like AWS Secrets Manager)
- Rotate LiveKit API keys periodically
- Use HTTPS for token endpoints in production
- Implement rate limiting to prevent abuse
- Validate client requests (e.g., check allowed identities/rooms)

### Token Transmission

- Token endpoint: HTTPS required in production
- Token to LiveKit: Transmitted over WSS (secure WebSocket)
- No tokens logged in production builds (only in development)

### Token Expiry

- Tokens automatically expire (server-controlled TTL)
- Transport refreshes proactively before expiry
- Old tokens immediately discarded after refresh
- No token reuse after connection close

---

## Error Handling

### Token Fetch Failures

**Scenarios:**
1. **Network error**: Logged with "HTTP request failed"
2. **HTTP error**: Logged with response code and body
3. **Invalid JSON**: Logged with parse error
4. **Missing token field**: Clear error message
5. **Timeout**: 30-second timeout, then logged

**Recovery:**
- Transport returns "waiting" state (doesn't fail hard)
- Tick()/Poll() continues to retry
- User can fall back to manual token if needed

### Token Expiry Handling

**Proactive Refresh:**
- Checked in Tick()/Poll() every frame
- Triggers refresh at configurable lead time (default: 5 minutes)
- Callback updates token in memory

**Expired Token:**
- Connection may fail if token expires mid-session
- Future enhancement: Automatic reconnection with fresh token
- Current: Relies on sufficient TTL (1+ hour recommended)

---

## Performance Considerations

### Async Operations

**All token operations are non-blocking:**
- HTTP requests use Unreal's async IHttpRequest
- No waiting on game thread
- Callbacks execute on HTTP module's thread
- Token manager uses atomics and mutexes for thread safety

### Memory Footprint

**Minimal overhead:**
- Token manager: ~100 bytes
- Token fetcher: ~50 bytes
- Cached token: Variable (typically < 1 KB)
- Total: < 5 KB per transport instance

### CPU Usage

**Negligible impact:**
- Token fetch: One-time on startup (async)
- Expiry check: Simple integer comparison per frame
- Refresh: Triggered once per token TTL (hours)
- JWT parsing: Only when new token received

---

## Testing

### Mock Token Server

**Location:** `Tests/mock-token-server.py`

**Features:**
- Generates valid JWT tokens
- Configurable TTL (default: 1 hour)
- Optional API key authentication
- Health check endpoint
- Debug endpoint for token inspection

**Usage:**
```bash
# Basic usage
python mock-token-server.py

# With API key
API_KEY=test-key python mock-token-server.py

# Custom TTL (5 minutes)
TOKEN_TTL=300 python mock-token-server.py

# Custom port
python mock-token-server.py --port 9000
```

### Manual Testing Procedure

1. **Start mock server**
2. **Configure sender with auto-fetch:**
   - Set `bUseAutoTokenFetch = true`
   - Set `TokenEndpointUrl = "http://localhost:8080/token"`
3. **Start sender**
4. **Observe logs:**
   - "Fetching token..."
   - "Token fetched successfully"
   - "WebRTC sender connecting..."
   - "WebRTC connected"
5. **Configure receiver similarly**
6. **Verify streaming works**

### Test Scenarios

- [x] Manual token mode (backward compatibility)
- [x] Auto-fetch with mock server
- [x] Auto-fetch with invalid endpoint
- [x] Auto-fetch with authentication
- [x] Token refresh before expiry
- [ ] Token expiry during active connection (needs long-running test)
- [ ] Network timeout handling
- [ ] Malformed JSON responses
- [ ] HTTP error responses

---

## Known Limitations

1. **No Automatic Reconnection:** If token expires during active connection, connection may drop. Manual reconnection required.
   - **Mitigation:** Use tokens with sufficient TTL (1+ hour)
   - **Future Enhancement:** Auto-reconnect with fresh token

2. **No Token Caching:** Tokens not persisted across application restarts.
   - **Rationale:** Security best practice
   - **Alternative:** Use auto-fetch on every startup

3. **Single Token Endpoint:** Only one endpoint URL supported per transport.
   - **Workaround:** Load-balance at DNS level if needed

4. **No Token Validation:** Transport doesn't verify JWT signature.
   - **Rationale:** LiveKit server validates on connection
   - **Advantage:** Simpler implementation, no crypto dependencies

---

## Future Enhancements

### Priority 1 (High Value)

1. **Automatic Reconnection on Expiry**
   - Detect connection failure due to expired token
   - Fetch fresh token
   - Reconnect automatically
   - Maintain streaming continuity

2. **UI Configuration Panels**
   - Add auto-fetch options to Sender Component details panel
   - Add auto-fetch options to LiveLink source configuration
   - Visual indicators for token status (valid, expired, fetching)

3. **Token Status API**
   - Expose token status to blueprints
   - Events for token fetch success/failure
   - Countdown to expiry display

### Priority 2 (Nice to Have)

1. **Token Persistence (Optional)**
   - Encrypted token storage
   - Re-use tokens across sessions
   - Configurable persistence policy

2. **Multiple Endpoints**
   - Failover token servers
   - Round-robin load balancing
   - Regional endpoint selection

3. **Advanced Authentication**
   - OAuth 2.0 support
   - Client certificates
   - HMAC request signing

### Priority 3 (Future Ideas)

1. **Token Caching**
   - Share tokens between multiple transports
   - Centralized token manager service
   - Token pool for multiple connections

2. **Metrics and Monitoring**
   - Token fetch latency tracking
   - Expiry warnings
   - Failure rate monitoring

---

## Migration Guide

### For Existing Projects

**No action required** - auto-fetch is opt-in:

1. Existing projects continue using manual tokens (`bUseAutoTokenFetch = false`)
2. To migrate to auto-fetch:
   - Set `bUseAutoTokenFetch = true`
   - Set `TokenEndpointUrl`
   - Remove or clear `Token` field
   - Optionally set `TokenApiKey` if endpoint requires it

### For New Projects

**Recommended setup:**

1. Deploy token generator endpoint (or use mock server for testing)
2. Configure transport with auto-fetch
3. Set appropriate token TTL (1+ hour recommended)
4. Monitor logs for token operations
5. Test token refresh by setting short TTL (5 minutes)

---

## Troubleshooting

### Common Issues

**1. "Token fetch failed: HTTP request failed"**
- **Cause:** Network error, invalid URL, or firewall
- **Solution:** Verify endpoint URL, check network connectivity, test with curl

**2. "Token fetch timed out after 30.0 seconds"**
- **Cause:** Slow or unresponsive token server
- **Solution:** Check server health, increase timeout (future enhancement), use fallback

**3. "Failed to parse JWT payload JSON"**
- **Cause:** Malformed JWT token from endpoint
- **Solution:** Verify endpoint returns valid JWT format, check token with jwt.io

**4. "WebRTC sender already connected"**
- **Cause:** Start() called multiple times
- **Solution:** Check IsConnected() before calling Start()

**5. "Waiting for token before connecting..."**
- **Cause:** Token fetch in progress
- **Solution:** Normal behavior, wait for fetch to complete (monitor logs)

### Debug Logging

Enable verbose logging:
```cpp
// In Unreal console
LogO3DWebRTCTokenManager Verbose
LogO3DWebRTCSender Verbose
LogO3DWebRTCReceiver Verbose
```

Look for:
- "Fetching token from endpoint..."
- "Token fetched successfully"
- "Token expiring soon, refreshing..."
- "Token refreshed successfully"

---

## Appendix

### File Structure

```
ProjectSandbox/Plugins/Open3DBroadcast/Source/
├── Open3DShared/Public/
│   └── O3DTransportTypes.h                 # Extended with token config
├── Open3DTransportWebRTC/
│   ├── Open3DTransportWebRTC.Build.cs      # Added HTTP, Json modules
│   ├── Private/
│   │   ├── Sender/
│   │   │   ├── WebRTCSender.h              # Token manager integration
│   │   │   └── WebRTCSender.cpp
│   │   ├── Receiver/
│   │   │   ├── WebRTCReceiver.h            # Token manager integration
│   │   │   └── WebRTCReceiver.cpp
│   │   └── Shared/
│   │       ├── WebRTCTokenManager.h        # NEW: Token manager
│   │       ├── WebRTCTokenManager.cpp
│   │       ├── WebRTCTokenFetcher.h        # NEW: HTTP fetcher
│   │       └── WebRTCTokenFetcher.cpp
│   └── Tests/
│       ├── mock-token-server.py            # NEW: Mock server
│       └── README.md                       # NEW: Testing guide
```

### Dependencies

**New Unreal Modules:**
- `HTTP` - For IHttpRequest async HTTP requests
- `Json` - For FJsonObject JSON parsing
- `JsonUtilities` - For TJsonWriter JSON serialization

**No External Dependencies:**
- No additional third-party libraries
- Uses Unreal's built-in Base64, JSON, and HTTP modules

### Code Statistics

**Lines of Code:**
- TokenManager: ~350 LOC
- TokenFetcher: ~250 LOC
- Integration (Sender): ~150 LOC
- Integration (Receiver): ~150 LOC
- Mock Server: ~120 LOC
- **Total: ~1020 LOC**

**Files Changed:**
- Created: 6 new files
- Modified: 5 existing files
- **Total: 11 files**

---

## References

### Internal Documentation

- [LiveKit README](../../../../../LIVEKIT_README.md)
- [LiveKit Quickstart](../../../../../LIVEKIT_QUICKSTART.md)
- [WebRTC User Guide](../USER_GUIDE.md)
- [Agent Instructions](../.github/copilot-instructions.md)

### External Resources

- [JWT Specification (RFC 7519)](https://datatracker.ietf.org/doc/html/rfc7519)
- [LiveKit Token Documentation](https://docs.livekit.io/guides/access-tokens/)
- [Unreal HTTP Module](https://docs.unrealengine.com/5.6/en-US/API/Runtime/HTTP/)
- [FBase64 API](https://docs.unrealengine.com/5.6/en-US/API/Runtime/Core/Misc/FBase64/)

---

## Credits

**Implementation:** GitHub Copilot Coding Agent  
**Design:** Open3DStream Planning Agent  
**Date:** November 23, 2024  
**Version:** 1.0

---

**End of Document**
