# Open3DTransportWebRTC - User Guide

A comprehensive guide to configuring, using, and troubleshooting the WebRTC transport module for Open3DStream.

## Table of Contents

1. [Quick Start](#quick-start)
2. [Configuration Guide](#configuration-guide)
   - [Credentials](#credentials)
   - [Automatic Token Fetch](#automatic-token-fetch-recommended)
3. [Audio Configuration](#audio-configuration)
4. [Platform Support](#platform-support)
5. [Troubleshooting](#troubleshooting)
6. [Performance Tuning](#performance-tuning)
7. [FAQ](#faq)

---

## Quick Start

The WebRTC transport allows you to stream motion capture data and audio to/from a LiveKit server, enabling multi-participant setups across networks.

### Basic Setup

1. **Obtain a LiveKit Server**
   - Use a cloud service (e.g., LiveKit Cloud, Liveblox)
   - Or self-host using Docker
   - Server URL will be in format: `wss://your-server.com`

2. **Generate Access Token**
   - Create a JWT token with Publisher or Subscriber role
   - Token includes: room name, identity, and permissions
   - Tokens typically expire in 24 hours (configurable on server)

3. **Configure Transport**
   ```
   URL: wss://your-server.com
   Token: <JWT token from step 2>
   ```
   The token is a credential. It is kept out of the level, the Blueprint, the ini files and LiveLink presets; see [Credentials](#credentials).

4. **Enable Audio (Optional)**
   - Check "Enable Audio" in transport settings
   - Select sample rate (48kHz recommended)
   - Choose bitrate based on bandwidth (see Audio section)

---

## Configuration Guide

### Server URL Format

**Required:** WebSocket Secure (WSS) protocol
- ✅ Correct: `wss://livekit.example.com`
- ❌ Wrong: `ws://livekit.example.com` (insecure, won't work)
- ❌ Wrong: `https://livekit.example.com` (wrong protocol)

**Port:** Usually 443 (default WSS), sometimes 7880 or other custom ports
- With port: `wss://livekit.example.com:7880`

### Access Token (JWT)

**What is it?**
- JSON Web Token (JWT) that authenticates you to the LiveKit server
- Grants permissions (Publisher/Subscriber) for specific rooms

**Generating Tokens**
- Use LiveKit CLI, SDK, or control panel
- Typical workflow:
  ```bash
  # Using LiveKit CLI
  livekit generate-token <api-key> <api-secret> \
    --room "MyRoom" \
    --identity "Sender1" \
    --grant-publisher \
    --ttl 3600  # 1 hour
  ```

**Token Expiration**
- ⚠️ Tokens have lifetimes (typically 24 hours default)
- When expired: Connection fails with authentication error
- **Solution:** Use automatic token fetch (recommended) or manually refresh tokens

### Credentials

The access token (`webrtc.token`) and the token endpoint credential (`webrtc.tokenEndpointAuth`) are secret options. The plugin never writes them to a level, a Blueprint, `GameUserSettings.ini`, a LiveLink connection string or preset, or a log.

**Where the value comes from**, in this order:
1. **This session:** the value typed into the password field in the editor, or set at runtime with the Blueprint node **Set Transport Secret** (`UO3DCredentialLibrary::SetTransportSecret`). It is gone after a restart.
2. **Environment variable:** `O3DB_WEBRTC_TOKEN` for the token, `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH` for the endpoint credential. With a credential profile other than `default`, `<NAME>__<PROFILE>` is tried first (for profile `stage`: `O3DB_WEBRTC_TOKEN__STAGE`). This works in packaged games and on render nodes.
3. **Remembered on this machine** (editor only): tick **Remember on this machine** next to the field. The value is stored in your per-user editor settings under `Saved/Config` (Base64-encoded, not encrypted), never in the project's `Config/` folder.

**In the editor panel**, each secret field opens empty. The line below it says where the current value comes from: "Not set", "Set for this session", "Remembered on this machine" or "From environment variable ...". **Clear** forgets the session value and the remembered copy; an environment variable still applies. With Auto Token Fetch off, a warning appears when no token is available.

**Credential Profile** (`webrtc.credentialProfile`, default `default`) is saved with the component or source. It names which stored credentials to use, so two senders can use different tokens without either token being saved.

**Upgrading older projects:** a level, Blueprint, `GameUserSettings.ini` or LiveLink preset saved by an older version may still contain a token. When it is loaded, the token is moved into this session's store and removed from the loaded data, and one warning names the asset or source (never the value). Resave the asset, or recreate the LiveLink source, so the token is removed from disk.

### Automatic Token Fetch (Recommended)

**New in v1.0.5:** Automatically fetch JWT tokens from your token server instead of manual entry.

**Benefits:**
- No manual token copying during development
- Tokens automatically refresh before expiration
- Supports multiple concurrent users with unique credentials
- LiveKit credentials stay secure on the server

**Setup:**

1. **Deploy a Token Generator Server**
   - Your backend service that generates LiveKit JWTs
   - Endpoint should accept POST requests to `/token`
   - Example: `https://your-server.com/token`
   - See `Tests/mock-token-server.py` for reference implementation

2. **Configure in Unreal Editor**
   - Select your O3DSenderComponent or open LiveLink source settings
   - Choose "WebRTC" as transport
   - Check "Use Auto Token Fetch"
   - Enter "Token Endpoint URL": `https://your-server.com/token`
   - Enter the "Token Endpoint Credential" your endpoint expects, or set `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH` (see [Credentials](#credentials))
   - Set "Room" to the same value on the sender and the receiver
   - Set "Token Refresh Lead Time": 300 seconds (5 minutes recommended)

3. **How It Works**
   - On startup: Unreal fetches token from your endpoint
   - Request includes: room name, identity, role (publisher/subscriber), and `Authorization: Bearer <endpoint credential>` when one is set
   - The request carries no grants; your server decides them
   - Your server generates and signs JWT using stored LiveKit credentials
   - Token automatically refreshes before expiration
   - No manual token management required

**Token endpoint requirements:**

The endpoint described here is a reference contract. `Tests/mock-token-server.py` implements it for local testing only. A real endpoint holds your LiveKit API secret, so anyone who can call it can join your rooms. It must:
- **Authenticate every caller.** The plugin sends `Authorization: Bearer <value>` from the `webrtc.tokenEndpointAuth` secret. An endpoint that answers without checking the caller hands out LiveKit tokens to anyone who can reach it.
- **Decide the grants itself** from the authenticated caller. The client sends room, identity and role as a request only; it does not send grants, and the endpoint must not accept grants from a client (a client that picks its own grants can publish).
- **Use HTTPS.** The plugin refuses a plain `http://` endpoint unless the host is `localhost`, `127.0.0.1` or `::1`.
- Keep the LiveKit API key and secret on the server.
- Accept POST with JSON: `{room, identity, role}`
- Return JSON: `{token, expiresAt}` or `{token, ttl}`

**Example Token Server Request/Response:**
```json
// Request
POST https://your-server.com/token
Content-Type: application/json
Authorization: Bearer <endpoint credential>
{
  "room": "MyRoom",
  "identity": "sender-12345",
  "role": "publisher"
}

// Response
{
  "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
  "expiresAt": 1700000000,
  "ttl": 3600
}
```

**Troubleshooting Auto Fetch:**
- Check logs: `LogO3DWebRTCTokenManager` for fetch status. Logs show the endpoint without its query string and never show the response body or the token
- "refused: use https://": the endpoint is plain `http://` on a host other than localhost
- HTTP 401 or 403: the endpoint rejected the credential; check the Token Endpoint Credential or `O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH`
- Verify endpoint URL is correct and accessible
- Ensure server returns valid JSON with "token" field
- Check network firewall allows HTTPS to your server
- Review retry attempts in logs (automatic retry on failures)

### Room Configuration

**Room Name**
- Manual token mode: encoded in the JWT token
- Auto Token Fetch: set the **Room** field (transport option `webrtc.room`) to the same value on the sender and the receiver. It is required; the transport does not start without it
- Multiple senders/receivers can join same room
- Different rooms are isolated (no crosstalk)

**Identity**
- Unique identifier for this participant in the room
- Auto Token Fetch generates one per sender or receiver instance (`sender-<pid>-<id>`, `receiver-<pid>-<id>`), so several in one editor do not replace each other

---

## Audio Configuration

### Sample Rate

**Recommended:** 48 kHz
- Standard for audio production
- Supported by LiveKit
- No resampling needed

**Other Options:** 44.1 kHz, 48 kHz, 96 kHz
- Higher rates require more bandwidth
- Most real-time audio uses 48 kHz

### Bitrate Selection

| Use Case | Bitrate | Quality | Bandwidth | Devices |
|----------|---------|---------|-----------|---------|
| **Voice Only** | 16 kbps | Acceptable | Very Low | Many |
| **Music/Effects** | 32 kbps | Good | Low | Good |
| **High Quality Audio** | 96 kbps | Excellent | Medium | Few |
| **Stereo Music** | 128 kbps | Excellent | Medium-High | Few |

**Default:** 24 kbps (good for voice + motion capture)

### Mono vs Stereo

| Mode | Channels | Use Case | Bitrate Impact |
|------|----------|----------|----------------|
| **Mono** | 1 | Voice, director cues, click track | Standard |
| **Stereo** | 2 | Music, ambience, spatial audio | ~1.5x bitrate |

**Recommendation:** Mono for motion capture, Stereo for music

### Audio Quality Troubleshooting

**Symptom: Audio Too Quiet**
- Increase input gain at microphone/capture
- Verify LiveKit audio levels in dashboard
- Check speaker volume

**Symptom: Audio Distorted/Clipping**
- Reduce microphone input gain
- Check for clipping indicators in audio software
- Note: The transport clamps float samples at ±1.0 (no automatic gain reduction)

**Symptom: Audio Dropout/Silence**
- Check bitrate vs available bandwidth
- Reduce skeleton complexity if data channel is saturated
- Monitor network latency
- Verify no audio sink configured (logs will show "frame discarded")

---

## Platform Support

### Current Status
- ✅ **Windows 64-bit** (Fully supported)
- ⏳ **Linux 64-bit** (Planned, binaries needed)
- ⏳ **macOS 64-bit** (Planned, binaries needed)

### Checking Platform
```
Windows: Any Windows 10/11 x64 system
Linux: Will fail with clear error message if attempted
macOS: Will fail with clear error message if attempted
```

### Alternative Transports
If WebRTC is not available for your platform:
- **NNG:** Flexible topology, good latency, LAN-ready
- **TCP:** Low-latency 1:1 streaming
- **UDP:** Ultra-low latency for LAN only
- **Loopback:** In-process testing and validation

---

## Troubleshooting

### Connection Failures

#### Error: "Connection failed (code=1)"
**Cause:** Invalid URL or server unreachable
**Solution:**
1. Verify URL format: `wss://server.com` (not `ws://` or `https://`)
2. Test connectivity: Ping server domain
3. Check firewall: Ensure port 443 (or custom WSS port) is open
4. Verify server is running

#### Error: "Connection failed (code=401)"
**Cause:** Invalid token, expired token, or permission mismatch
**Solution:**
1. Verify token format (should be valid JWT)
2. Check token expiration: Tokens have TTL (default 24 hours)
3. Verify token grants correct permissions (Publisher/Subscriber)
4. Generate new token from server control panel

#### Error: "Connection timeout"
**Cause:** Network latency, firewall blocking, or server overload
**Solution:**
1. Check network latency: Should be <200ms typically
2. Verify firewall allows WebSocket (WSS) outbound
3. Check server load and capacity
4. Try connecting to different server region

### Data Transfer Issues

#### Frames Being Dropped
**Symptoms:** Send() returns false, DroppedFrames counter increases

**Cause 1: Not Connected**
- Verify `GetStats().bConnected` is true
- Wait for connection to complete (async)

**Cause 2: Payload Too Large**
- Complex skeletons can exceed 1300 bytes (lossy limit)
- Check warning: "Payload size exceeds lossy limit"
- **Solutions:**
  1. Simplify skeleton (remove unused bones)
  2. Reduce bone precision
  3. Transport automatically switches to reliable channel for 1300-15000 byte payloads
  4. If payload > 15KB, frame is rejected (ERROR log)

#### Latency Higher Than Expected
**Expected Range:** 20-100ms (mostly network RTT)
- SFU adds ~5-10ms overhead
- Network round-trip time dominates

**Solutions:**
1. Use lower-latency network (wired > WiFi)
2. Choose closer server region
3. Reduce frame rate if not needed
4. Monitor network conditions

### Audio Issues

#### "Failed to publish audio" Warning
**Cause 1:** Not connected to server
- Wait for connection callback before submitting audio

**Cause 2:** Invalid audio parameters
- NumChannels: Must be 1 or 2
- SampleRate: Must be valid (typically 48000)
- NumFrames: Must be > 0

**Cause 3:** Audio bitrate out of range
- Valid range: 16-128 kbps
- If outside range: Clamped silently (check logs)

#### Audio Frame Discarded (No Sink)
**Message:** "WebRTC audio frame discarded (no sink)"
**Cause:** No audio sink registered
**Solution:** Call `SetAudioSink()` before starting receiver

---

## Performance Tuning

### Data Send Rate

**Default:** 60 frames/second

**For Heavy Scenes (Many Bones):**
- Reduce to 30 fps
- Reduces data throughput by 50%
- May require skeleton optimization

```
TargetDataSendHz = 30  // Instead of 60
```

### Bandwidth Estimation

**Per Second Data Rate:**
```
Data Rate = (Payload Size in bytes) × (Send Rate in Hz)

Example:
  Payload = 5 KB (5000 bytes)
  Send Rate = 60 Hz
  Data Rate = 5000 × 60 = 300,000 bytes/sec = 2.4 Mbps
```

**Plus Audio:**
```
Audio Rate = (Bitrate in kbps) / 8
Example: 96 kbps = 12 KB/s = 96 Kbps

Total = Data Rate + Audio Rate
```

### CPU Optimization

**Sender CPU Usage:**
- ~3-5% on modern CPU (Intel i7+, AMD Ryzen 5+)
- Primarily WebRTC encoding and network I/O
- Audio conversion: <0.1% (negligible)

**Receiver CPU Usage:**
- ~2-3% per receiver (less than sender)
- Mostly network I/O and deserialization

**If CPU is High:**
1. Reduce skeleton complexity
2. Reduce send/receive frame rate
3. Check for CPU-intensive consumer (application side)

### Network Optimization

**Test Your Network:**
```bash
# Quick latency test to server
ping your-server.com

# Bandwidth test (if available)
iperf -c your-server.com
```

**Optimize for Bandwidth:**
- Reduce skeleton bone count
- Reduce send frequency (60 fps → 30 fps)
- Reduce audio bitrate (96 kbps → 32 kbps)
- Simplify animation (LOD)

**Optimize for Latency:**
- Use wired network (vs WiFi)
- Choose nearest LiveKit server
- Reduce frame rate (30 fps may feel smoother with lower latency)
- Monitor round-trip time (RTT)

---

## FAQ

### Q: Can I use WebRTC on Linux?
**A:** Currently No (Windows 64-bit only). LibKit FFI binaries for Linux are on the roadmap. Use TCP, UDP, or NNG transports as alternatives.

### Q: How long are tokens valid?
**A:** Depends on server configuration, typically 24 hours. Check your LiveKit admin panel. Tokens can be refreshed before expiration.

### Q: Can multiple senders use same room?
**A:** Yes. All senders with same room name and Publisher role can send simultaneously. Receivers subscribe to all publishers in the room.

### Q: What if I can't reach the server?
**A:**
1. Check server is running
2. Verify URL format (WSS, not WS)
3. Check firewall allows WebSocket
4. Ping server domain to verify DNS
5. Try different LiveKit server (may be down)

### Q: How much bandwidth do I need?
**A:** Depends on skeleton complexity and audio:
- Minimal (simple rig, no audio): 0.5-2 Mbps
- Typical (complex rig, voice): 2-5 Mbps
- High quality (music): 5-10 Mbps

### Q: Can I switch transports at runtime?
**A:** No. Choose transport at application startup. To switch, stop current transport and initialize new one.

### Q: Does WebRTC work through corporate firewalls?
**A:** Usually yes. WebRTC uses STUN/TURN for NAT traversal. If direct connection fails, TURN relay provides fallback. However, some very restrictive firewalls may block all P2P. Consult your network administrator.

### Q: What is the latency range?
**A:** 20-100ms is typical:
- 5-10ms: Application processing
- 10-50ms: Network transit to SFU
- 5-10ms: SFU relay
- 10-50ms: Network transit to receiver
- Plus occasional buffering/jitter

### Q: Can I use self-signed certificates?
**A:** Not recommended. Use proper SSL certificates from trusted CA. Self-signed may work in development but will fail in production due to browser/client security restrictions.

### Q: How do I monitor performance?
**A:** Call `GetStats()` to retrieve:
```
Stats.FramesSent         // Total frames sent
Stats.FramesReceived     // Total frames received
Stats.BytesSent          // Total bytes sent
Stats.BytesReceived      // Total bytes received
Stats.DroppedFrames      // Frames rejected (too large, not connected)
Stats.AverageLatencyMs   // Average round-trip latency
Stats.MaxLatencyMs       // Peak latency observed
```

### Q: Can I rate-limit data sending?
**A:** Yes, reduce `TargetDataSendHz` from 60 to 30 (or lower). The transport framework handles pacing.

---

## Additional Resources

- **LiveKit Documentation:** https://docs.livekit.io
- **Open3DStream Documentation:** See parent README
- **Report Issues:** GitHub issues in Open3DStream repository

---

**Last Updated:** 2025-11-15
**Version:** 1.0

