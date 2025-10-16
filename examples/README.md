# Open3DStream Examples

This directory contains example implementations and utilities for Open3DStream.

## Signaling Server (signaling-server.js)

A simple Node.js WebSocket-based signaling server for WebRTC connections.

### Requirements
- Node.js 12+
- npm or yarn

### Installation
```bash
cd examples
npm install ws
```

### Usage
```bash
# Start on default port (8080)
node signaling-server.js

# Start on custom port
node signaling-server.js 9000
```

### Testing
```bash
# Health check
curl http://localhost:8080/health

# Expected response:
# {"status":"ok","rooms":0,"timestamp":"2025-10-16T..."}
```

### Configuration

The signaling server supports:
- **Room-based peer grouping**: Clients join rooms by ID
- **SDP relay**: Offers and answers forwarded between peers
- **ICE candidate relay**: NAT traversal candidate exchange
- **Peer notifications**: Join/leave events

### Message Protocol

**Join Room:**
```json
{
  "type": "join",
  "roomId": "myroom",
  "name": "Client1"
}
```

**Send Offer/Answer:**
```json
{
  "type": "offer",
  "sdp": "...",
  "roomId": "myroom"
}
```

**Send ICE Candidate:**
```json
{
  "type": "candidate",
  "candidate": "...",
  "mid": "0",
  "roomId": "myroom"
}
```

### Production Deployment

For production use, consider:
- Adding authentication/authorization
- Using TLS/WSS for secure connections
- Implementing rate limiting
- Adding logging and monitoring
- Scaling with Redis for multi-server deployments

## Future Examples

- [ ] Python signaling server
- [ ] Browser-based animation viewer
- [ ] WebRTC sender application
- [ ] Performance test harness
- [ ] Multi-room stress test

## Contributing

When adding examples:
1. Include clear documentation
2. Add installation/usage instructions
3. Provide sample output
4. Keep dependencies minimal
5. Test on multiple platforms
