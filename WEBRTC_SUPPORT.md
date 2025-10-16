# WebRTC Support for Open3DStream

## Overview

Open3DStream now supports WebRTC as a network transport protocol, enabling low-latency, peer-to-peer streaming of animation data with built-in NAT traversal. This is particularly useful for:

- Cloud-based motion capture streaming
- Remote collaboration scenarios
- Browser-based animation clients
- Scenarios requiring firewall/NAT traversal
- Peer-to-peer animation data sharing

## Architecture

The WebRTC implementation uses **libdatachannel**, a lightweight C++ library that provides:
- WebRTC data channels for binary data transmission
- STUN/TURN support for NAT traversal
- WebSocket-based signaling
- Modern C++ API

### Components

1. **WebRTCClient**: Client-side connector that connects to a signaling server and establishes peer-to-peer data channels
2. **WebRTCServer**: Server-side connector (future implementation for broadcast scenarios)
3. **Signaling Server**: Separate component for WebRTC peer discovery and SDP/ICE exchange

## Building with WebRTC Support

### Prerequisites

1. **libdatachannel** library
2. **nlohmann/json** for JSON parsing
3. CMake 3.13 or higher

### Installation

#### Linux/macOS
```bash
# Install libdatachannel
git clone https://github.com/paullouisageneau/libdatachannel.git
cd libdatachannel
cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0
cmake --build build
sudo cmake --install build

# Build Open3DStream with WebRTC
cd /path/to/Open3DStream
mkdir build && cd build
cmake .. -DO3DS_ENABLE_WEBRTC=ON
make -j4
```

#### Windows
```cmd
REM Install libdatachannel via vcpkg
vcpkg install libdatachannel

REM Build Open3DStream
cd C:\path\to\Open3DStream
mkdir build && cd build
cmake .. -DO3DS_ENABLE_WEBRTC=ON -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

### CMake Options

- `-DO3DS_ENABLE_WEBRTC=ON`: Enable WebRTC support (default: OFF)
- `-DLibDataChannel_DIR=/path/to/lib`: Specify libdatachannel installation path

## Usage

### URL Format

```
webrtc://signaling-server:port/room-id
```

**Examples:**
- `webrtc://localhost:8080/myroom`
- `webrtc://signal.example.com:9000/session123`
- `webrtc://192.168.1.100:8080/capture-stream`

### Unreal LiveLink Configuration

1. **Add LiveLink Source**
   - Open LiveLink window in Unreal
   - Click "+ Source"
   - Select "Open3DStream Source"

2. **Configure Connection**
   - **URL**: `webrtc://your-signaling-server:port/room-id`
   - **Protocol**: Select "WebRTC Client"
   - **Key**: (optional) Authentication key

3. **Connect**
   - Click "Create"
   - Monitor status in LiveLink window

### C++ API Usage

```cpp
#include "o3ds/webrtc_connector.h"

// Create WebRTC client
O3DS::WebRTCClient client;

// Set callback for incoming data
client.setFunc(this, [](void* ctx, void* data, size_t len) {
    // Handle received animation data
    auto* buffer = static_cast<char*>(data);
    // Process O3DS frames...
});

// Configure ICE servers (optional)
client.setIceServers(
    "stun:stun.l.google.com:19302",  // STUN server
    "turn:turn.example.com:3478",    // TURN server (optional)
    "username",                       // TURN username
    "password"                        // TURN password
);

// Connect to signaling server and establish WebRTC connection
if (client.start("webrtc://localhost:8080/myroom")) {
    std::cout << "WebRTC connection established" << std::endl;
} else {
    std::cerr << "Failed to connect: " << client.getError() << std::endl;
}

// Send animation data
std::vector<char> animationData = /* ... */;
client.write(animationData.data(), animationData.size());

// Cleanup
client.stop();
```

## Signaling Server

WebRTC requires a signaling server for peer discovery and connection negotiation. You can use any WebSocket-based signaling server that handles:

- Room-based peer grouping
- SDP offer/answer exchange
- ICE candidate relay

### Simple Node.js Signaling Server

```javascript
const WebSocket = require('ws');
const wss = new WebSocket.Server({ port: 8080 });

const rooms = new Map();

wss.on('connection', (ws) => {
    let currentRoom = null;
    
    ws.on('message', (message) => {
        const data = JSON.parse(message);
        
        if (data.type === 'join') {
            currentRoom = data.roomId;
            if (!rooms.has(currentRoom)) {
                rooms.set(currentRoom, new Set());
            }
            rooms.get(currentRoom).add(ws);
        } else {
            // Relay signaling messages to room peers
            if (currentRoom && rooms.has(currentRoom)) {
                rooms.get(currentRoom).forEach((peer) => {
                    if (peer !== ws && peer.readyState === WebSocket.OPEN) {
                        peer.send(JSON.stringify(data));
                    }
                });
            }
        }
    });
    
    ws.on('close', () => {
        if (currentRoom && rooms.has(currentRoom)) {
            rooms.get(currentRoom).delete(ws);
        }
    });
});

console.log('Signaling server running on ws://localhost:8080');
```

Save as `signaling-server.js` and run:
```bash
npm install ws
node signaling-server.js
```

## NAT Traversal Configuration

### STUN Servers (Public)
Free STUN servers for testing:
- `stun:stun.l.google.com:19302`
- `stun:stun1.l.google.com:19302`
- `stun:stun2.l.google.com:19302`

### TURN Servers (For Restricted Networks)
For production use behind strict firewalls, configure a TURN server:

```cpp
client.setIceServers(
    "stun:stun.l.google.com:19302",
    "turn:your-turn-server.com:3478",
    "your-username",
    "your-password"
);
```

**Popular TURN Server Options:**
- **coturn**: Open-source TURN server
- **Twilio STUN/TURN**: Cloud-based service
- **xirsys**: Managed TURN service

## Performance Considerations

### Bandwidth
- WebRTC data channels support configurable bandwidth
- Typical O3DS streaming: 100-500 KB/s
- Curve data adds minimal overhead

### Latency
- WebRTC provides <100ms latency in ideal conditions
- TURN relay adds 20-50ms
- Optimized for real-time animation streaming

### Connection Limits
- Signaling server: Unlimited rooms
- Peer connections: Typically 10-50 per client
- Data channel: One per peer connection

## Troubleshooting

### Connection Fails

**Check signaling server:**
```bash
curl -i -N -H "Connection: Upgrade" -H "Upgrade: websocket" \
     -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: test" \
     http://localhost:8080/ws
```

**Verify ICE candidates:**
- Check firewall allows UDP traffic
- Ensure STUN server is reachable
- Configure TURN if behind symmetric NAT

### No Data Received

**Verify data channel state:**
- Check `isOpen()` on data channel
- Monitor connection state changes
- Check for ICE connection failures

**Enable verbose logging:**
```cpp
// In webrtc_connector.cpp, add logging
rtc::InitLogger(rtc::LogLevel::Debug);
```

### High Latency

- Use STUN instead of TURN when possible
- Choose geographically close STUN/TURN servers
- Optimize network path (direct peer-to-peer is fastest)
- Check for packet loss

## Security Considerations

### Encryption
- WebRTC data channels use DTLS encryption (enabled by default)
- All data is encrypted end-to-end

### Authentication
- Implement signaling server authentication
- Use room-based access control
- Consider TURN server credentials

### Network Security
- WebRTC can bypass some firewall rules
- Implement proper access controls on signaling server
- Monitor and limit concurrent connections

## Comparison with Other Protocols

| Feature | WebRTC | TCP | UDP | WebSocket |
|---------|--------|-----|-----|-----------|
| NAT Traversal | ✓ Built-in | ✗ | ✗ | ✗ |
| Encryption | ✓ DTLS | ✗ | ✗ | Optional TLS |
| Latency | Low | Medium | Lowest | Medium |
| Reliability | Configurable | High | Low | High |
| Setup Complexity | High | Low | Low | Medium |
| Browser Support | ✓ | ✗ | ✗ | ✓ |

## Future Enhancements

- [ ] WebRTC Server implementation for broadcast scenarios
- [ ] Automatic ICE server discovery
- [ ] Connection quality monitoring
- [ ] Adaptive bitrate based on network conditions
- [ ] Multi-stream support (separate channels for skeleton/curves)
- [ ] Browser-based animation client example

## References

- [libdatachannel Documentation](https://github.com/paullouisageneau/libdatachannel)
- [WebRTC Specification](https://www.w3.org/TR/webrtc/)
- [STUN/TURN Protocols](https://tools.ietf.org/html/rfc5389)
- [ICE Protocol](https://tools.ietf.org/html/rfc5245)

## Support

For WebRTC-specific issues:
1. Check libdatachannel GitHub issues
2. Verify signaling server is running
3. Test with public STUN servers first
4. Enable debug logging for detailed diagnostics

For Open3DStream integration:
- Review existing protocol implementations (TCP, UDP)
- Check base_connector interface compatibility
- Verify callback setup in UOpen3DServer.cpp