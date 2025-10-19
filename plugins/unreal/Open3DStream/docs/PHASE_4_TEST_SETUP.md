# Phase 4 - WebRTC Testing Setup Guide

## ✅ Current Status

**Date**: October 17, 2025  
**Branch**: `phase-4-webrtc-testing`  
**Signaling Server**: Running on GitHub Codespaces

## 🌐 Network Setup

### Signaling Server Location
- **Environment**: GitHub Codespaces (remote Linux container)
- **Codespace**: `stunning-giggle-p55rjxrw4wc7gq7`
- **Port**: 8080
- **Health Check**: `https://stunning-giggle-p55rjxrw4wc7gq7-8080.app.github.dev/health`

### Port Forwarding
GitHub Codespaces automatically forwards port 8080. To verify:
1. Open the **PORTS** tab in VS Code (bottom panel)
2. Find port 8080
3. Ensure visibility is **Public**
4. Copy the forwarded URL

## 🎮 Unreal Editor Testing (Windows)

### Step 1: Open Project
- Navigate to: `ProjectSandbox\ProjectSandbox.uproject`
- Open in Unreal Editor 5.4+ or 5.5+

### Step 2: Open LiveLink
- Menu: **Window → Virtual Production → Live Link**

### Step 3: Add Open3DStream Source
- Click **+ Source** button
- Select **Open3DStream Source**

### Step 4: Configure WebRTC Connection

**URL Format**:
```
webrtc://<codespace-name>-8080.app.github.dev/testroom
```

**For This Session**:
```
webrtc://stunning-giggle-p55rjxrw4wc7gq7-8080.app.github.dev/testroom
```

**Settings**:
- **Protocol**: WebRTC Client
- **Room**: testroom (or any name you choose)

### Step 5: Verify Connection

**In Unreal Output Log** (Window → Developer Tools → Output Log):
Look for:
```
✅ WebRTC Connector: Starting connection (Mode: Client)
✅ WebRTC Connector: Parsed URL - Host: ..., Port: 8080, Room: testroom, Protocol: wss
✅ Signaling client: Connected to wss://...
✅ Signaling client: Joined room "testroom"
✅ WebRTC: Peer connection established
✅ WebRTC: Data channel "Open3DStream" opened
```

**In Signaling Server Console** (Codespaces terminal):
```
[2025-10-17T...] Client abc123def connected from ::ffff:...
[2025-10-17T...] unnamed joined room "testroom" (1 peers)
```

**In LiveLink UI**:
- Source should show **green "Active"** status
- Subject list will be empty until data is sent

## 🐛 Troubleshooting

### Connection Refused
- **Check**: Is port 8080 forwarded in PORTS tab?
- **Check**: Is port visibility set to **Public**?
- **Check**: Is signaling server running? Test health endpoint in browser

### SSL/TLS Errors
- **Fixed**: Code now auto-detects `wss://` for remote hosts
- **Verify**: Check Unreal log shows `Protocol: wss` not `ws`

### No Peer Connection
- **Expected**: If no data sender is running yet
- **Message**: "Waiting for peer in room 'testroom'"
- **Solution**: Continue to Phase 4 Task 3 (end-to-end test with data sender)

### Wrong URL Format
- ❌ `ws://localhost:8080/testroom` (won't work - server is remote)
- ❌ `wss://...app.github.dev/testroom` (wrong scheme - use `webrtc://`)
- ✅ `webrtc://...app.github.dev/testroom` (correct!)

## 🔧 Code Changes Made

### File: `WebRTCConnector.cpp` (Line 56-67)

**Before**:
```cpp
SignalingServerUrl = FString::Printf(TEXT("ws://%s:%d"), *Host, Port);
```

**After**:
```cpp
// Use wss:// for remote hosts (HTTPS/secure), ws:// for localhost
FString Protocol = TEXT("ws");
if (!Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) && 
    !Host.Equals(TEXT("127.0.0.1")) && 
    !Host.StartsWith(TEXT("192.168.")) &&
    !Host.StartsWith(TEXT("10.")))
{
    Protocol = TEXT("wss");
}
SignalingServerUrl = FString::Printf(TEXT("%s://%s:%d"), *Protocol, *Host, Port);
```

**Reason**: GitHub Codespaces uses HTTPS/WSS for forwarded ports. The connector must detect remote hosts and use secure WebSocket protocol.

## 📋 Test Checklist

### Task 2: Connection Test (Current)
- [ ] Open Unreal Editor on Windows
- [ ] Configure LiveLink with WebRTC source
- [ ] Use forwarded Codespaces URL
- [ ] Verify signaling connection in logs
- [ ] Check peer connection established
- [ ] Confirm data channel opened
- [ ] Monitor signaling server console

### Task 3: Data Streaming Test (Next)
- [ ] Start Python data sender (listener.py)
- [ ] Configure sender with same room name
- [ ] Verify binary data received in Unreal
- [ ] Check LiveLink shows animation subjects
- [ ] Test skeleton transforms
- [ ] Test morph target curves

## 📝 Notes

- **WSS Support**: Now works with GitHub Codespaces and production deployments
- **Local Testing**: Still supports `ws://` for localhost development
- **STUN Servers**: Using default Google STUN (configured in WebRTCConnector)
- **Data Channel**: Named "Open3DStream", reliable ordered delivery

## 🚀 Next Steps

1. **Test Connection**: Follow steps above to verify WebRTC handshake
2. **Create Data Sender**: Modify Python listener.py to send via WebRTC
3. **End-to-End Test**: Verify animation data flows from sender → signaling → Unreal
4. **Error Handling**: Test disconnect/reconnect scenarios
5. **Documentation**: Update WEBRTC_QUICKSTART.md with findings

---

**Testing Started**: October 17, 2025  
**Signaling Server PID**: 329334  
**Server Health**: http://localhost:8080/health (internal) or https://...app.github.dev/health (public)
