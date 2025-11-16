# Open3DTransportWebRTC - User Guide

A comprehensive guide to configuring, using, and troubleshooting the WebRTC transport module for Open3DStream.

## Table of Contents

1. [Quick Start](#quick-start)
2. [Configuration Guide](#configuration-guide)
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
- **Solution:** Refresh token before expiration or request new one from server

### Room Configuration

**Room Name**
- Encoded in JWT token
- Multiple senders/receivers can join same room
- Different rooms are isolated (no crosstalk)

**Identity**
- Unique identifier for this participant in the room
- Used for display and tracking
- Example: "Sender1", "Receiver_Studio_A"

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

