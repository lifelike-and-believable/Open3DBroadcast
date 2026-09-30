# Open3DTransportWebRTC Testing Tools

This directory contains testing utilities for the WebRTC transport module.

## Mock Token Server

### Overview

`mock-token-server.py` is a simple HTTP server that generates JWT tokens for testing the token auto-fetch functionality. It mimics the behavior of a LiveKit token generator without requiring a full LiveKit server deployment.

It is a reference for the request and response contract, not a production token service. A real token endpoint must authenticate its callers and decide their grants itself; see "Token endpoint requirements" in `../USER_GUIDE.md`.

### Quick Start

1. Install dependencies:
   ```bash
   pip install flask pyjwt
   ```

2. Start the server. `API_SECRET` is required; the server exits without it. Setting `API_KEY` is strongly recommended:
   ```bash
   API_SECRET=<signing-secret> API_KEY=<endpoint-credential> python mock-token-server.py
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

- `API_SECRET` (required): Secret for signing JWTs. There is no default; the server exits when it is unset.
- `LIVEKIT_API_KEY`: JWT issuer (`iss`, default `mock-token-server`). A real LiveKit server only accepts tokens whose issuer is one of its API keys and whose signature uses that key's secret, so set `LIVEKIT_API_KEY` and `API_SECRET` to the server's key pair (`devkey` / `secret` for `livekit-server --dev`). With the defaults, the tokens only work against code that does not verify them.
- `API_KEY`: Credential callers must send as `Authorization: Bearer <API_KEY>`. Without it, anyone who can reach the server gets a token. In Unreal, enter the same value as the **Token Endpoint Credential** (secret option `webrtc.tokenEndpointAuth`) or set `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH`.
- `TOKEN_TTL`: Token lifetime in seconds (default: 3600 = 1 hour)

The server decides grants from the requested `role` (`publisher` or `subscriber`; anything else is a 400). A `grants` object sent by a client is ignored.

### Examples

#### With API Key Authentication (Recommended)

With `API_KEY` set, the mock server requires a bearer credential, as a production token server must.

```bash
# Start server with API key requirement
API_SECRET=<signing-secret> API_KEY=my-secret-key python mock-token-server.py

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
API_SECRET=<signing-secret> TOKEN_TTL=300 python mock-token-server.py
```

#### Custom Port and Host

```bash
API_SECRET=<signing-secret> python mock-token-server.py --host 0.0.0.0 --port 9000
```

The plugin refuses plain `http://` token endpoints unless the host is `localhost`, `127.0.0.1` or `::1`. To reach the mock server from another machine, put it behind HTTPS.

### Integration with Unreal

To use the mock server with Open3DTransportWebRTC:

1. Start the mock server:
   ```bash
   API_SECRET=<signing-secret> API_KEY=my-secret-key python mock-token-server.py
   ```

2. In Unreal Editor, configure your sender/receiver:
   - Enable "Use Auto Token Fetch"
   - Set "Token Endpoint URL" to `http://localhost:8080/token` (plain `http://` is accepted only for localhost)
   - Enter `my-secret-key` as the "Token Endpoint Credential", or set `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH=my-secret-key` before starting the editor
   - Set "Room" to the same room name on both sides (e.g., "test-room")

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
- Set `API_SECRET`; the server exits with an error without it
- Ensure Flask and PyJWT are installed: `pip install flask pyjwt`
- Check if port 8080 is already in use: `lsof -i :8080` (Unix) or `netstat -ano | findstr :8080` (Windows)

**Token generation fails:**
- Check that request has valid JSON body
- Verify Authorization header if API_KEY is set (a 401 means the credential is missing or wrong)
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
