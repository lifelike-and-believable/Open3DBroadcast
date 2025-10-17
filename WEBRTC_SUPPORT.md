# WebRTC support for Open3DStream

## Overview

Open3DStream supports WebRTC as a network transport protocol, enabling low-latency, peer-to-peer streaming of animation data with built-in NAT traversal. This is particularly useful for:

- Cloud-based motion capture streaming
- Remote collaboration scenarios
- Browser-based animation clients
- Scenarios requiring firewall/NAT traversal
- Peer-to-peer animation data sharing

## Architecture

The WebRTC implementation uses libdatachannel, a lightweight C++ library that provides:
- WebRTC data channels for binary data transmission
- STUN/TURN support for NAT traversal
- WebSocket-based signaling
- Modern C++ API

### Components

1. WebRTCClient: Client-side connector that connects to a signaling server and establishes peer-to-peer data channels.
2. WebRTCServer: Not yet implemented (future broadcast scenarios). Use client mode today.
3. Signaling server: A small WebSocket server used for peer discovery and SDP/ICE exchange.

## Building with WebRTC support

### Prerequisites

1. CMake 3.13 or higher
2. A C++17 compiler
3. No external libdatachannel install required for typical builds: Open3DStream bundles and links against libdatachannel (and mbedtls) via its thirdparty configuration and prebuilt binaries.

### Installation

#### All platforms

```bash
# Build Open3DStream (WebRTC is enabled by default)
mkdir -p build && cd build
cmake ..
make -j4

# To explicitly toggle
cmake .. -DO3DS_ENABLE_WEBRTC=ON   # enable (default)
cmake .. -DO3DS_ENABLE_WEBRTC=OFF  # disable
```

### CMake options

- -DO3DS_ENABLE_WEBRTC=ON: Enable WebRTC support (default: ON)
- -DLibDataChannel_DIR=/path/to/lib: Only needed when using a custom libdatachannel build. By default, Open3DStream uses the bundled/prebuilt libs.

## Usage

### URL format

```
webrtc://signaling-server:port/room-id
```

Examples:
- `webrtc://localhost:8080/myroom`
- `webrtc://signal.example.com:9000/session123`
- `webrtc://192.168.1.100:8080/capture-stream`

### Unreal LiveLink configuration

1. **Add LiveLink Source**
   - Open LiveLink window in Unreal
   - Click "+ Source"
   - Select "Open3DStream Source"

2. Configure connection
    - URL: `webrtc://your-signaling-server:port/room-id`
    - Protocol: Select "WebRTC Client"
    - Note: Unreal’s WebRTC path is functional via libdatachannel and marked Beta. It uses a lightweight WebSocket signaling server.

3. **Connect**
   - Click "Create"
   - Monitor status in LiveLink window

### C++ API usage

```cpp
// WebRTC API is compiled when O3DS_ENABLE_WEBRTC is ON
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

## Signaling server

WebRTC requires a signaling server for peer discovery and connection negotiation. You can use any WebSocket-based signaling server that handles:

- Room-based peer grouping
- SDP offer/answer exchange
- ICE candidate relay

### Simple Node.js signaling server

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

Save as `examples/signaling-server.js` (already in repo) and run:
```bash
npm install ws
node signaling-server.js
```

## NAT traversal configuration

### STUN servers (public)
Free STUN servers for testing:
- `stun:stun.l.google.com:19302`
- `stun:stun1.l.google.com:19302`
- `stun:stun2.l.google.com:19302`

### TURN servers (for restricted networks)
For production use behind strict firewalls, configure a TURN server:

```cpp
client.setIceServers(
    "stun:stun.l.google.com:19302",
    "turn:your-turn-server.com:3478",
    "your-username",
    "your-password"
);
```

Popular TURN server options:
- **coturn**: Open-source TURN server
- **Twilio STUN/TURN**: Cloud-based service
- **xirsys**: Managed TURN service

## Performance considerations

### Bandwidth
- WebRTC data channels support configurable bandwidth
- Typical O3DS streaming: 100-500 KB/s
- Curve data adds minimal overhead

### Latency
- WebRTC provides <100ms latency in ideal conditions
- TURN relay adds 20-50ms
- Optimized for real-time animation streaming

### Connection limits
- Signaling server: Unlimited rooms
- Peer connections: Typically 10-50 per client
- Data channel: One per peer connection

## Troubleshooting

### Connection fails

Check signaling server:
```bash
curl -i -N -H "Connection: Upgrade" -H "Upgrade: websocket" \
     -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: test" \
     http://localhost:8080/ws
```

Verify ICE candidates:
- Check firewall allows UDP traffic
- Ensure STUN server is reachable
- Configure TURN if behind symmetric NAT

### No data received

Verify data channel state:
- Check `isOpen()` on data channel
- Monitor connection state changes
- Check for ICE connection failures

Enable verbose logging:
```cpp
// In webrtc_connector.cpp, add logging
rtc::InitLogger(rtc::LogLevel::Debug);
```

### High latency

- Use STUN instead of TURN when possible
- Choose geographically close STUN/TURN servers
- Optimize network path (direct peer-to-peer is fastest)
- Check for packet loss

## Security considerations

### Encryption
- WebRTC data channels use DTLS encryption (enabled by default)
- All data is encrypted end-to-end

### Authentication
- Implement signaling server authentication
- Use room-based access control
- Consider TURN server credentials

### Network security
- WebRTC can bypass some firewall rules
- Implement proper access controls on signaling server
- Monitor and limit concurrent connections

## Comparison with other protocols

| Feature | WebRTC | TCP | UDP |
|---------|--------|-----|-----|
| NAT traversal | ✓ Built-in | ✗ | ✗ |
| Encryption | ✓ DTLS | ✗ | ✗ |
| Latency | Low | Medium | Lowest |
| Reliability | Configurable | High | Low |
| Setup complexity | Medium | Low | Low |

## Future enhancements

- [ ] WebRTC server implementation for broadcast scenarios
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
- Verify callback setup in `UOpen3DServer.cpp`