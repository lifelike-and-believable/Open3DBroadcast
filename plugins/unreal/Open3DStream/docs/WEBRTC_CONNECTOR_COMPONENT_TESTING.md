# WebRTC Connector Component Testing Guide

## Purpose

`UO3DSWebRTCConnectorComponent` is a standalone test harness for the `IWebRTCConnector` interface and `LibDataChannelConnector` implementation. **Always test this component in isolation after any changes to the connector interfaces or implementations before integrating with broadcaster/receiver code.**

## Component Overview

- **Location:** `plugins/unreal/Open3DStream/Source/Open3DBroadcast/Public/O3DSWebRTCConnectorComponent.h`
- **Purpose:** Minimal ActorComponent that wraps `IWebRTCConnector` for standalone testing
- **Use Cases:**
  - Verify WebRTC connection establishment
  - Test DataChannel send/receive
  - Validate audio RTP packet flow
  - Debug signaling protocol issues
  - Confirm URL normalization behavior

## Quick Start

### 1. Setup Signaling Server

```powershell
cd E:\OtherProjects\libdatachannel\examples\signaling-server-python
py .\signaling-server.py 8080
```

### 2. Create Test Actors

**Server Actor:**
1. Create empty Actor in level
2. Add `O3DSWebRTCConnectorComponent`
3. Configure properties:
   - **SignalingUrl:** `ws://127.0.0.1:8080`
   - **bServer:** `true`
   - **Room:** `test-room`
   - **bEnableAudio:** `false` (for basic DataChannel test)
   - **bVerbose:** `true`
   - **LocalId:** Leave empty (will use Room as path)

**Client Actor:**
1. Create empty Actor in level
2. Add `O3DSWebRTCConnectorComponent`
3. Configure properties:
   - **SignalingUrl:** `ws://127.0.0.1:8080`
   - **bServer:** `false`
   - **Room:** `test-room`
   - **bEnableAudio:** `false`
   - **bVerbose:** `true`
   - **LocalId:** `client` (optional, auto-appended if missing)

### 3. Run and Verify

**Expected Output Log (Output Log window):**

```
[ExampleConnector] State: SignalingConnected
[ExampleConnector] State: DataChannelOpen
[ExampleConnector] Data: [68 65 6C 6C 6F ...] (28 bytes)
```

**Expected Signaling Server Output:**

```
Client test-room connected
Client client?room=test-room connected
Client client?room=test-room << { "id": "test-room", "type": "offer", ... }
Client test-room >> { "id": "client", "type": "answer", ... }
```

### 4. Verify Connection

- **Both actors should log `State: DataChannelOpen`**
- **Client sends "hello from example component" → Server receives it**
- **Server responds → Client receives it**
- Use CVars to inspect data:
  ```
  o3ds.WebRTCExample.LogDataContent 1
  o3ds.WebRTCExample.LogDataCount 1
  ```

## Testing Checklist After Connector Changes

When modifying `IWebRTCConnector`, `LibDataChannelConnector`, or signaling logic:

- [ ] **Update O3DSWebRTCConnectorComponent** if interface changed
- [ ] **Basic connectivity:** Server + Client establish DataChannel
- [ ] **URL normalization:** Server registers as clean room name (no `role=` suffix)
- [ ] **DataChannel send/receive:** Both directions work
- [ ] **Audio RTP flow:** If `bEnableAudio=true`, verify `OnRemoteAudioRtp` fires
- [ ] **Debug tone:** If `bSendDebugTone=true`, verify RTP packets sent
- [ ] **Verbose logging:** Trace state transitions
- [ ] **Error handling:** Disconnect signaling server mid-session, verify graceful handling

## Audio Testing

To test audio RTP flow:

1. Set **bEnableAudio:** `true` on both actors
2. Set **bSendDebugTone:** `true` on client
3. Set **ToneHz:** `440.0`
4. Set **ToneDurationSec:** `1.0`
5. Run PIE
6. Check Output Log for:
   ```
   [ExampleConnector] RTP: 1234 bytes
   ```

## Troubleshooting

### "Client test-room not found"
- **Cause:** Server registered with query parameters in path (e.g., `test-room?role=server`)
- **Fix:** Ensure `LibDataChannelConnector` strips `role=` and `room=` from query when constructing server path ID

### "DataChannelOpen" never fires
- Check signaling server is running
- Verify both actors use same `Room` value
- Enable `bVerbose=true` and check for ICE candidate exchange logs

### No audio RTP packets
- Ensure `bEnableAudio=true` on both server and client
- Verify audio track negotiated in SDP (check signaling server logs for `m=audio` line)
- Confirm `bSendDebugTone=true` on sender side

## CVars Reference

```
o3ds.WebRTCExample.LogDataContent 1    # Log first 64 bytes of DataChannel payloads (hex)
o3ds.WebRTCExample.LogDataCount 1      # Log byte count only for DataChannel receives
```

## Success Criteria

✅ **Pass:** Both actors connect, exchange DataChannel messages, disconnect cleanly  
✅ **Pass:** Server registers as clean room name (no query params in path)  
✅ **Pass:** Audio RTP packets flow when `bEnableAudio=true`  
❌ **Fail:** Connection timeout, "client not found" errors, RTP silence when audio enabled

---

**Last Updated:** 2025-11-03  
**Related:** WEBRTC_TESTING_GUIDE.md (full receiver/broadcaster workflow)
