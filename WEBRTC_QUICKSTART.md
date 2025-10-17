# WebRTC Quick Start Guide

> Note: This guide covers WebRTC usage in C++ command-line tools. For Unreal Engine, the plugin includes a functional WebRTC connector (beta) using libdatachannel with a simple WebSocket signaling server. See WEBRTC_UNREAL_IMPLEMENTATION.md and Issue #15 for details.

## 5-Minute Setup (C++ Tools)

### Step 1: Install Dependencies

```bash
# Install libdatachannel
git clone https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel
cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0
cmake --build build -j4
sudo cmake --install build
```

### Step 2: Build Open3DStream with WebRTC

```bash
cd /workspaces/Open3DStream
mkdir -p build && cd build
cmake .. -DO3DS_ENABLE_WEBRTC=ON
make -j4
```

### Step 3: Start Signaling Server

```bash
cd /workspaces/Open3DStream/examples
npm install ws
node signaling-server.js
```

Expected output:

```text
Open3DStream WebRTC Signaling Server
Listening on ws://localhost:8080/ws
Health check: http://localhost:8080/health

Ready to accept WebRTC signaling connections
```

### Step 4: Configure Unreal LiveLink

1. Open Unreal Engine
2. Window → Virtual Production → Live Link
3. Click "+ Source"
4. Select "Open3DStream Source"
5. Configure:
   - **URL**: `webrtc://localhost:8080/myroom`
   - **Protocol**: Select "WebRTC Client"
6. Click "Create"

### Step 5: Verify Connection

Check signaling server console for:

```text
[2025-10-16T...] Client abc123def connected from ::ffff:127.0.0.1
[2025-10-16T...] unnamed joined room "myroom" (1 peers)
```

## Common Commands

### Health Check

```bash
curl http://localhost:8080/health
```

### Test WebSocket Connection

```bash
npm install -g wscat
wscat -c ws://localhost:8080/ws
```

### Build without WebRTC

```bash
cmake .. -DO3DS_DISABLE_WEBRTC=ON
make -j4
```

## Troubleshooting

### libdatachannel not found

```bash
export CMAKE_PREFIX_PATH=/usr/local:$CMAKE_PREFIX_PATH
cmake .. -DO3DS_ENABLE_WEBRTC=ON
```

### Signaling server won't start

```bash
# Check if port 8080 is in use
lsof -i :8080
# Use different port
node signaling-server.js 9000
```

### No WebRTC protocol in Unreal

- Verify build completed with `-DO3DS_ENABLE_WEBRTC=ON` (default)
- Check CMake output for LibDataChannel found
- Rebuild Unreal plugin

## URL Examples

### Local Testing

```text
webrtc://localhost:8080/test-room
```

### Network Testing

```text
webrtc://192.168.1.100:8080/capture-01
```

### Remote Server

```text
webrtc://signal.mycompany.com:8080/production-stream
```

## STUN/TURN Configuration

### Default (Google STUN)

Already configured in code - no action needed.

### Custom STUN

```cpp
client.setIceServers("stun:stun.myserver.com:3478");
```

### With TURN

```cpp
client.setIceServers(
    "stun:stun.myserver.com:3478",
    "turn:turn.myserver.com:3478",
    "username",
    "password"
);
```

## Next Steps

- [Full WebRTC Documentation](WEBRTC_SUPPORT.md)
- [Implementation Details](WEBRTC_IMPLEMENTATION_SUMMARY.md)
- [Signaling Server Customization](examples/README.md)
