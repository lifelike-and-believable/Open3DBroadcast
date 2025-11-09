# Issue M0.4 - Transport Role Pairing Documentation

> **Milestone**: M0 - Protocol and Role Alignment  
> **Area**: transport, networking, documentation  
> **Priority**: High  
> **Estimated Effort**: 2-3 days  
> **Dependencies**: M0.1 (Protocol Message Types and Versioning Confirmation)

## Context and Purpose

Open3DBroadcast will send animation data to remote receivers (Unreal LiveLink Source, Maya, MotionBuilder) over various transports: TCP, UDP, NNG (multiple modes), and optionally WebRTC. Each transport has client/server roles or pub/sub patterns, and the sender (broadcast) must use the complementary role to the receiver.

This issue documents the exact role pairing for each transport, ensuring unambiguous configuration and successful connections. The goal is to prevent common mistakes like "both sides are clients" or "both are servers."

## Tasks

### 1. Review Transport Implementations in Core Library
- [ ] Study TCP implementation:
  - [`src/o3ds/tcp.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/tcp.cpp)
  - TCP Client mode (connects to server)
  - TCP Server mode (listens for connections)
  - Document connection establishment flow for each role
- [ ] Study UDP implementation:
  - [`src/o3ds/udp.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/udp.cpp)
  - [`src/o3ds/udp_fragment.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/udp_fragment.cpp)
  - UDP "Client" (sends to destination IP:port)
  - UDP "Server" (binds to port, receives from any)
  - Note: UDP is connectionless; clarify terminology
- [ ] Study NNG connector implementations:
  - [`src/o3ds/nng_connector.h`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/nng_connector.h)
  - [`src/o3ds/publisher.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/publisher.cpp) - Publish/Subscribe pattern
  - [`src/o3ds/subscriber.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/subscriber.cpp)
  - [`src/o3ds/pair.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/pair.cpp) - Pair pattern (bidirectional)
  - Request/Reply pattern (if used)
  - Pipeline pattern (if used)
  - Document which NNG patterns are suitable for broadcast use case
- [ ] Study connector overview:
  - [`sphinx/connectors.rst`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/sphinx/connectors.rst)
  - Summarize connector capabilities and role options

### 2. Review LiveLink Source Receiver Options
- [ ] Examine Unreal LiveLink Source configuration in `plugins/unreal/Open3DStream/Source/Open3DStream/`:
  - What transport options does it offer in the UI?
  - What roles does it support (client, server, subscriber, etc.)?
  - Document URL format for each transport
- [ ] Create table mapping LiveLink Source options to their roles:

  | LiveLink Source UI Option | Actual Role | URL Example |
  |---------------------------|-------------|-------------|
  | TCP Client | Connects to server | `tcp://server-ip:port` |
  | TCP Server | Listens for connections | `tcp://0.0.0.0:port` or `tcp://*:port` |
  | UDP Client | Sends to destination | `udp://dest-ip:port` |
  | UDP Server | Binds to port | `udp://0.0.0.0:port` or `udp://*:port` |
  | NNG Subscribe | Subscribes to publisher | `nng+tcp://publisher-ip:port` |
  | NNG Client | Connects to NNG server | `nng+tcp://server-ip:port` |
  | NNG Server | Listens for NNG clients | `nng+tcp://0.0.0.0:port` |
  | (add more as discovered) | | |

### 3. Review Maya and MotionBuilder Receiver Configurations (if applicable)
- [ ] Check Maya plugin receiver options (`plugins/maya/`):
  - What transports and roles does Maya receiver support?
  - Document Maya-specific URL formats or configuration
- [ ] Check MotionBuilder plugin receiver options (`plugins/mobu/`):
  - What transports and roles does MotionBuilder receiver support?
  - Document MotionBuilder-specific configuration
- [ ] Note any differences from Unreal LiveLink Source

### 4. Create Transport Pairing Matrix
- [ ] Expand and validate the transport pairing table from `Open3DBroadcast_Planning.md`:

  | Receiver Option | Receiver Role | Broadcast Role | Who Connects | Who Listens | Notes |
  |-----------------|---------------|----------------|--------------|-------------|-------|
  | TCP Client | Client | Server | Receiver → Broadcast | Broadcast | Broadcast listens on port; LiveLink connects |
  | TCP Server | Server | Client | Broadcast → Receiver | Receiver | LiveLink listens; Broadcast connects |
  | UDP "Server" | Binds to port | "Client" sends | N/A (connectionless) | Receiver binds | Broadcast sends to receiver IP:port |
  | UDP "Client" | Sends to dest | "Server" binds | N/A | Broadcast binds | Receiver sends to broadcast IP:port (unusual for broadcast use case) |
  | NNG Subscribe | Subscriber | Publisher | Subscriber → Publisher | Publisher | Broadcast publishes; LiveLink subscribes |
  | NNG Client | Client | Server | Client → Server | Server | Broadcast listens; LiveLink dials |
  | NNG Server | Server | Client | Client → Server | Server | LiveLink listens; Broadcast dials |
  | WebRTC Client | Client | Server | (via signaling) | (via signaling) | Optional/Beta; both use signaling server |
  | WebRTC Server | Server | Client | (via signaling) | (via signaling) | Optional/Beta |

- [ ] Clarify terminology:
  - "Client" typically dials/connects
  - "Server" typically listens/accepts
  - "Publisher" broadcasts to subscribers
  - "Subscriber" receives from publishers
  - UDP is connectionless; terms are for clarity only

### 5. Document URL Formats and Configuration
- [ ] Specify URL format for each transport mode from Broadcast perspective:

  | Transport | Broadcast Role | URL Format | Example | Notes |
  |-----------|----------------|------------|---------|-------|
  | TCP | Server | `tcp://*:port` or `tcp://0.0.0.0:port` | `tcp://*:9000` | Listens on all interfaces |
  | TCP | Server (specific interface) | `tcp://ip:port` | `tcp://192.168.1.100:9000` | Listens on specific IP |
  | TCP | Client | `tcp://server-ip:port` | `tcp://192.168.1.50:9000` | Connects to receiver server |
  | UDP | "Client" (sender) | `udp://dest-ip:port` | `udp://192.168.1.50:9001` | Sends to receiver address |
  | UDP | "Server" (binds) | `udp://*:port` or `udp://0.0.0.0:port` | `udp://*:9001` | Binds to port, receives |
  | NNG Publish | Publisher | `nng+tcp://*:port` or `nng+ipc://path` | `nng+tcp://*:9002` | Publishes; subscribers connect |
  | NNG Server | Server | `nng+tcp://*:port` | `nng+tcp://*:9003` | Listens; clients dial |
  | NNG Client | Client | `nng+tcp://server-ip:port` | `nng+tcp://192.168.1.50:9003` | Dials server |
  | WebRTC | Client/Server | `webrtc://signaling-host:port/room` | `webrtc://localhost:8080/broadcast` | Via signaling; optional |

- [ ] Document configuration parameters:
  - IP address: `*` or `0.0.0.0` for all interfaces, specific IP to bind to one interface
  - Port: numeric port number (1024-65535 recommended)
  - Room/topic: for WebRTC or pub/sub patterns
  - Optional parameters: STUN/TURN for WebRTC, fragmentation settings for UDP

### 6. Create Common Configuration Scenarios
- [ ] Document typical use cases:
  
  **Scenario 1: Local loopback test**
  - Broadcast: TCP Server on `tcp://*:9000`
  - LiveLink: TCP Client to `tcp://localhost:9000`
  - Notes: Both on same machine, no firewall issues
  
  **Scenario 2: LAN streaming**
  - Broadcast: TCP Server on `tcp://*:9000` (or specific interface IP)
  - LiveLink: TCP Client to `tcp://broadcast-machine-ip:9000`
  - Notes: Ensure firewall allows incoming TCP on port 9000
  
  **Scenario 3: UDP broadcast (one-to-many)**
  - Broadcast: UDP "Client" to `udp://receiver-ip:9001` (or multicast)
  - Multiple LiveLink: UDP "Server" on `udp://*:9001`
  - Notes: No handshake, may lose packets, efficient for one-to-many
  
  **Scenario 4: NNG pub/sub (one-to-many)**
  - Broadcast: NNG Publisher on `nng+tcp://*:9002`
  - Multiple LiveLink: NNG Subscriber to `nng+tcp://broadcast-ip:9002`
  - Notes: Subscribers connect to publisher, reliable delivery
  
  **Scenario 5: WebRTC over internet (optional/beta)**
  - Signaling Server: `node examples/signaling-server.js` on port 8080
  - Broadcast: WebRTC Client to `webrtc://signaling-host:8080/room1`
  - LiveLink: WebRTC Server to `webrtc://signaling-host:8080/room1`
  - Notes: NAT traversal via STUN, optional TURN for difficult networks

- [ ] Document troubleshooting for each scenario:
  - Connection refused: Check role pairing, firewall, IP address
  - No data received: Check role pairing, verify sender is active
  - Intermittent connection: UDP packet loss, network congestion
  - WebRTC connection fails: STUN/TURN config, signaling server issues

### 7. Document WebRTC Transport (Optional/Beta)
- [ ] Review WebRTC documentation:
  - [`WEBRTC_IMPLEMENTATION_SUMMARY.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_IMPLEMENTATION_SUMMARY.md)
  - [`WEBRTC_IMPLEMENTATION_COMPLETE.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_IMPLEMENTATION_COMPLETE.md)
  - [`WEBRTC_SUPPORT.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_SUPPORT.md)
  - [`LIBDATACHANNEL_INTEGRATION.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/LIBDATACHANNEL_INTEGRATION.md)
  - [`ISSUE_8_IMPLEMENTATION.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_8_IMPLEMENTATION.md)
- [ ] Document WebRTC role pairing:
  - How does signaling server mediate connection?
  - What are "client" and "server" roles in WebRTC context?
  - Does one side create offer, other creates answer?
- [ ] Note WebRTC as optional/beta:
  - Mark as experimental until wider validation
  - Document build flag to enable/disable (if applicable)
  - Reference signaling server example: `examples/signaling-server.js`

### 8. Create Transport Selection Guide for Broadcast Users
- [ ] Create decision guide (e.g., `docs/BROADCAST_TRANSPORT_GUIDE.md` or section in main docs):
  
  **When to use each transport:**
  
  | Transport | Use When | Pros | Cons |
  |-----------|----------|------|------|
  | TCP | Reliable LAN/local streaming | Reliable, in-order, connection-aware | Higher latency, requires pairing |
  | UDP | Low-latency, one-to-many, lossy OK | Low latency, multicast possible | Packet loss, no ordering guarantees |
  | NNG Pub/Sub | One-to-many, reliable | Reliable, auto-reconnect, pattern flexibility | Requires NNG library |
  | NNG Pair | Bidirectional, single receiver | Bidirectional, simple | One-to-one only |
  | WebRTC | Internet, NAT traversal, secure | NAT traversal, encrypted, low latency | Complex setup, signaling required, beta |

- [ ] Provide recommendations:
  - Local testing: TCP loopback (simplest)
  - LAN production: TCP or NNG Pub/Sub (reliable)
  - Low-latency local: UDP (accept some packet loss)
  - Internet/remote: WebRTC (when ready) or VPN with TCP/NNG
  - One-to-many: UDP or NNG Pub/Sub

### 9. Validate Pairing Matrix with Tests
- [ ] For each transport/role combination in the matrix:
  - [ ] Verify connection can be established (at least document expected behavior)
  - [ ] Note if testing is deferred to M3 (transport integration milestone)
- [ ] Document any combinations that are invalid or not recommended:
  - E.g., "UDP Client to UDP Client" doesn't make sense
  - E.g., "TCP Server to TCP Server" won't connect

## Acceptance Criteria

- [ ] **Transport Pairing Matrix Complete**: Clear table mapping receiver options to broadcast roles for all supported transports
- [ ] **URL Format Documented**: Explicit URL syntax for each transport and role
- [ ] **Common Scenarios Provided**: At least 3-5 typical configurations with step-by-step setup
- [ ] **Troubleshooting Guide**: Common connection issues and solutions for each transport
- [ ] **Transport Selection Guide**: Recommendations on when to use which transport
- [ ] **WebRTC Documented**: Optional/beta WebRTC transport documented with caveats
- [ ] **Validation**: Matrix entries are logically correct (even if not all tested yet)
- [ ] **Reference Document Created**: Comprehensive transport guide exists (e.g., `docs/BROADCAST_TRANSPORT_GUIDE.md`)

## Out of Scope

- Implementing transport code (that's M3)
- Performance testing or benchmarking transports (that's M8)
- Multicast UDP configuration (note as future feature)
- Custom transport protocols beyond TCP/UDP/NNG/WebRTC
- Signaling server implementation for WebRTC (use existing example)

## References

### Core Transport Code
- TCP: [`src/o3ds/tcp.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/tcp.cpp)
- UDP: [`src/o3ds/udp.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/udp.cpp), [`src/o3ds/udp_fragment.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/udp_fragment.cpp)
- NNG: [`src/o3ds/nng_connector.h`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/nng_connector.h), [`src/o3ds/publisher.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/publisher.cpp), [`src/o3ds/subscriber.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/subscriber.cpp), [`src/o3ds/pair.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds/pair.cpp)

### Documentation
- Connectors overview: [`sphinx/connectors.rst`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/sphinx/connectors.rst)
- Build sources: [`src/CMakeLists.txt`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/CMakeLists.txt)

### WebRTC (Optional/Beta)
- [`WEBRTC_IMPLEMENTATION_SUMMARY.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_IMPLEMENTATION_SUMMARY.md)
- [`WEBRTC_IMPLEMENTATION_COMPLETE.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_IMPLEMENTATION_COMPLETE.md)
- [`WEBRTC_SUPPORT.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/WEBRTC_SUPPORT.md)
- [`LIBDATACHANNEL_INTEGRATION.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/LIBDATACHANNEL_INTEGRATION.md)
- [`ISSUE_8_IMPLEMENTATION.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/ISSUE_8_IMPLEMENTATION.md)

### Unreal Receiver
- LiveLink Source: `plugins/unreal/Open3DStream/Source/Open3DStream/`

### Maya and MotionBuilder Receivers
- Maya plugin: `plugins/maya/`
- MotionBuilder plugin: `plugins/mobu/`

### Planning Context
- [`Open3DBroadcast_Planning.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md) (M0 section, transport pairing table)

## Success Metrics

1. **Clarity**: No ambiguity about which broadcast role pairs with which receiver option
2. **Completeness**: All transports and roles documented
3. **Usability**: Non-experts can configure correct pairing from the guide
4. **Troubleshooting**: Common mistakes are anticipated and solutions provided

## Risks and Considerations

- **NNG Complexity**: NNG supports many patterns (pub/sub, req/rep, pair, pipeline); document only those relevant to broadcast
- **WebRTC Maturity**: WebRTC is marked optional/beta; clearly indicate this in docs
- **Firewall Issues**: Most connection problems are firewall-related; emphasize firewall configuration
- **URL Format Variations**: Different connectors may accept slightly different URL formats; document precisely
- **Multicast UDP**: Could be valuable for one-to-many but adds complexity; defer or mark as future
- **Receiver Diversity**: Maya/MotionBuilder may have different capabilities; document differences clearly

## Example Pairing Checklist (for user validation)

When setting up broadcast, users should verify:

- [ ] I know my receiver's transport option (e.g., "TCP Client")
- [ ] I've found the complementary broadcast role in the pairing matrix (e.g., "TCP Server")
- [ ] I've configured the broadcast URL correctly (e.g., `tcp://*:9000`)
- [ ] I've configured the receiver URL correctly (e.g., `tcp://broadcast-ip:9000`)
- [ ] Firewall allows traffic on the chosen port (if TCP/UDP)
- [ ] If WebRTC, signaling server is running and reachable
- [ ] I've tested connection with minimal data first

## Next Steps After Completion

Once this issue is complete, the M0 milestone is finished and developers can proceed with:
- **M1**: Single-mesh capture implementation (protocol foundation is complete)
- **M3**: Transport integration implementation (will use this pairing guide)
- **M6**: Editor UI configuration (will reference transport selection guide)

---

**Labels**: `milestone:M0`, `area:transport`, `area:networking`, `area:documentation`, `good-first-task`  
**Assignee**: (TBD - suitable for coding agent familiar with networking and transport protocols)
