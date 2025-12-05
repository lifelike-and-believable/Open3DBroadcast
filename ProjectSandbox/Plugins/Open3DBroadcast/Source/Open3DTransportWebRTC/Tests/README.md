# Open3DTransportWebRTC Testing Tools

This directory contains testing utilities for the WebRTC transport module.

## Table of Contents

- [Mock Token Server](#mock-token-server) - Python server for testing auto-fetch
- [Unit Tests](#unit-tests) - Comprehensive test suite

---

## Unit Tests

The WebRTC transport module includes comprehensive unit tests for the token auto-fetch functionality. Tests are located in `Private/Tests/WebRTCTokenTests.cpp`.

### Test Categories

| Category | Test Count | Description |
|----------|------------|-------------|
| JWT Parsing | 5 | Token manager correctly parses JWT expiry claims |
| Configuration | 3 | Manual vs auto-fetch mode initialization |
| Thread Safety | 1 | Callback queuing for concurrent requests |
| Request Building | 3 | Token fetcher constructs correct HTTP requests |
| Integration | 4 | Sender/receiver with auto-fetch config |
| Backward Compat | 2 | Existing manual token projects still work |
| Error Handling | 4 | Timeout, reset, and cancellation |
| Config | 2 | Role and refresh lead time settings |

### Running Tests

Tests run through Unreal Engine's automation testing framework:

```powershell
# Run all WebRTC token tests
Build/Scripts/Run-AutomationTests.ps1 -TestFilter="Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch"

# Run specific test category
Build/Scripts/Run-AutomationTests.ps1 -TestFilter="Open3DBroadcast.Open3DTransportWebRTC.TokenAutoFetch.Manager"
```

### Test Design Principles

1. **No server required** - Tests validate logic without needing a live LiveKit server
2. **JWT generation** - Helper functions create valid JWT tokens for testing
3. **Failure simulation** - Tests cover network errors, timeouts, and malformed responses
4. **Platform awareness** - Windows-64bit tests are appropriately guarded

---

## Mock Token Server

### Overview

`mock-token-server.py` is a simple HTTP server that generates JWT tokens for testing the token auto-fetch functionality. It mimics the behavior of a LiveKit token generator without requiring a full LiveKit server deployment.

### Quick Start

1. Install dependencies:
   ```bash
   pip install flask pyjwt
   ```

2. Start the server:
   ```bash
   python mock-token-server.py
   ```

3. The server will start on `http://localhost:8080`

### Usage

#### Generate a Token

```bash
curl -X POST http://localhost:8080/token \
  -H "Content-Type: application/json" \
  -d '{
    "room": "test-room",
    "identity": "sender-1",
    "role": "publisher"
  }'
```

Response:
```json
{
  "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "expiresAt": 1234567890,
  "ttl": 3600
}
```

#### Health Check

```bash
curl http://localhost:8080/health
```

### Configuration

#### Command Line Options

- `--host HOST`: Host to bind to (default: localhost)
- `--port PORT`: Port to listen on (default: 8080)
- `--debug`: Enable Flask debug mode

#### Environment Variables

- `API_KEY`: Expected API key for authentication (optional, default: none)
  - If set, requests must include `Authorization: Bearer <API_KEY>` header
- `API_SECRET`: Secret for signing JWTs (default: "test-secret")
- `TOKEN_TTL`: Token lifetime in seconds (default: 3600 = 1 hour)

### Examples

#### With API Key Authentication (Optional)

The mock server can optionally require authentication. This simulates a production token server that validates requests before generating tokens.

```bash
# Start server with API key requirement
API_KEY=my-secret-key python mock-token-server.py

# Request token with authentication
curl -X POST http://localhost:8080/token \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer my-secret-key" \
  -d '{"room":"test-room","identity":"sender-1","role":"publisher"}'
```

**Note:** This authentication is between the client and the token server, NOT LiveKit credentials. The LiveKit API credentials are stored in the mock server's `API_SECRET` environment variable and never sent by the client.

#### Custom Token TTL

```bash
# Generate tokens that expire in 5 minutes
TOKEN_TTL=300 python mock-token-server.py
```

#### Custom Port and Host

```bash
python mock-token-server.py --host 0.0.0.0 --port 9000
```

### Integration with Unreal

To use the mock server with Open3DTransportWebRTC:

1. Start the mock server:
   ```bash
   python mock-token-server.py
   ```

2. In Unreal Editor, configure your sender/receiver:
   - Enable "Use Auto Token Fetch"
   - Set "Token Endpoint URL" to `http://localhost:8080/token`
   - Set "Stream ID" to your room name (e.g., "test-room")

3. The transport will automatically fetch tokens from the mock server

**Security Note:** The mock server represents the token generator service that stores LiveKit API credentials. In production, this would be a secure backend service. The Unreal client only sends room, identity, and role information - it never has access to LiveKit API credentials.

### Token Format

The mock server generates JWT tokens with the following structure:

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
  "exp": 1234567890,
  "iss": "mock-token-server",
  "sub": "sender-1",
  "nbf": 1234564290,
  "video": {
    "room": "test-room",
    "roomJoin": true,
    "roomCreate": true,
    "canPublish": true,
    "canSubscribe": false
  },
  "metadata": "{\"role\":\"publisher\"}"
}
```

### Troubleshooting

**Server won't start:**
- Ensure Flask and PyJWT are installed: `pip install flask pyjwt`
- Check if port 8080 is already in use: `lsof -i :8080` (Unix) or `netstat -ano | findstr :8080` (Windows)

**Token generation fails:**
- Check that request has valid JSON body
- Verify Authorization header if API_KEY is set
- Check server logs for specific error messages

**Unreal can't connect to server:**
- Ensure server is running: `curl http://localhost:8080/health`
- Check firewall settings
- Verify endpoint URL is correct (http://localhost:8080/token)

### Development

The mock server is intentionally simple for easy debugging and modification. Key features:

- No database or persistent storage
- Stateless token generation
- Minimal dependencies
- Clear logging of token requests

To customize token payload or add new endpoints, edit `mock-token-server.py` directly.
