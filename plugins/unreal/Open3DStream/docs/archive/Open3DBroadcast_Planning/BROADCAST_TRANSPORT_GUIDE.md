# Open3DBroadcast — Transport Role Pairing and Selection (M0.4)

This guide documents exactly how to pair Broadcast and Receiver roles across supported transports (TCP, UDP, NNG, WebRTC), the URL formats to use, and common configuration scenarios.

Related references:

- Connector layer overview: `sphinx/connectors.rst`
- Core connectors and patterns: `src/o3ds/` (tcp, udp, nng, webrtc integration)
- Unreal LiveLink receiver: `plugins/unreal/Open3DStream/Source/Open3DStream/`
- Planning: `ISSUE_M0_4_TRANSPORT_ROLES.md`

## Transport pairing matrix

| Receiver Option | Receiver Role | Broadcast Role | Who Connects | Who Listens | Notes |
|-----------------|---------------|----------------|--------------|-------------|-------|
| TCP Client      | Client        | Server         | Receiver → Broadcast | Broadcast | Broadcast listens on port; LiveLink connects |
| TCP Server      | Server        | Client         | Broadcast → Receiver | Receiver  | LiveLink listens; Broadcast connects |
| UDP "Server"    | Binds/Recv    | Sender         | N/A          | Receiver    | Connectionless; Broadcast sends to receiver IP:port |
| UDP "Client"    | Sends         | Binds/Recv     | N/A          | Broadcast   | Unusual for broadcast use; included for completeness |
| NNG Subscribe   | Subscriber    | Publisher      | Subscriber → Publisher | Publisher | Broadcast publishes; multiple receivers subscribe |
| NNG Server      | Server        | Client         | Client → Server | Server | Broadcast dials; LiveLink listens |
| NNG Client      | Client        | Server         | Client → Server | Server | Broadcast listens; LiveLink dials |
| WebRTC Client   | Client (offer/answer via signaling) | Server     | Via signaling | Via signaling | Optional/Beta; roles mediated by signaling |
| WebRTC Server   | Server (offer/answer via signaling) | Client     | Via signaling | Via signaling | Optional/Beta |

Terminology: Client dials, Server listens; Publisher broadcasts, Subscriber consumes. UDP is connectionless—roles describe send/bind behavior.

## URL formats (Broadcast perspective)

| Transport | Broadcast Role | URL Format | Example | Notes |
|-----------|----------------|-----------|---------|-------|
| TCP       | Server         | `tcp://*:port` or `tcp://ip:port` | `tcp://*:9000` | Listens on all/specific interface |
| TCP       | Client         | `tcp://receiver-ip:port` | `tcp://192.168.1.50:9000` | Connects to receiver |
| UDP       | Sender         | `udp://dest-ip:port` | `udp://192.168.1.50:9001` | Sends to receiver address |
| UDP       | Binds/Recv     | `udp://*:port` | `udp://*:9001` | Rare for broadcast use |
| NNG       | Publisher      | `nng+tcp://*:port` or `nng+ipc://path` | `nng+tcp://*:9002` | Subscribers dial |
| NNG       | Server         | `nng+tcp://*:port` | `nng+tcp://*:9003` | Clients dial |
| NNG       | Client         | `nng+tcp://server-ip:port` | `nng+tcp://192.168.1.50:9003` | Dials server |
| WebRTC    | Client/Server  | `webrtc://signaling-host:port/room` | `webrtc://localhost:8080/room1` | Uses signaling; ICE/STUN/TURN configured externally |

Notes:

- Use `*` or `0.0.0.0` to listen on all interfaces; use a specific IP to bind a single NIC.
- For NNG IPC, provide a valid local path on supported OSes (Windows named pipes/Unix domain sockets variants).
- WebRTC requires signaling; do not hard-code TURN credentials.

## Common scenarios

1) Local loopback test

- Broadcast: TCP Server on `tcp://*:9000`
- LiveLink: TCP Client to `tcp://localhost:9000`
- Notes: Simple, reliable; firewall usually not an issue locally

1) LAN streaming

- Broadcast: TCP Server on `tcp://*:9000` (or specific IP)
- LiveLink: TCP Client to `tcp://broadcast-ip:9000`
- Notes: Open firewall inbound on 9000/TCP on sender

1) One-to-many via NNG pub/sub

- Broadcast: `nng+tcp://*:9002` (Publisher)
- Receivers: `nng+tcp://broadcast-ip:9002` (Subscriber)
- Notes: Reliable delivery to multiple receivers; auto-reconnect built-in

1) Low-latency via UDP (loss-tolerant)

- Broadcast: `udp://receiver-ip:9001`
- Receivers: `udp://*:9001`
- Notes: No ordering/reliability; consider fragmentation settings in core if payload is large

1) WebRTC over internet (beta)

- Signaling: `node examples/signaling-server.js` (e.g., 8080)
- Broadcast: `webrtc://signaling-host:8080/room1`
- Receiver: `webrtc://signaling-host:8080/room1`
- Notes: Ensure STUN/TURN configured; encrypted channels; roles are mediated by signaling/offer-answer

## Troubleshooting

- Connection refused: Check complementary roles (client connects to server), IP/port, and firewall
- No data on UDP: Verify correct dest IP/port; remember UDP is connectionless
- NNG not connecting: Validate URL scheme (`nng+tcp://` vs `nng+ipc://`), ensure publisher/server started first
- WebRTC fails: Verify signaling server reachability and ICE configuration; avoid hard-coded TURN creds

## Transport selection guide

| Transport | Use When | Pros | Cons |
|-----------|----------|------|------|
| TCP | Reliable LAN/local | Reliable, in-order | Higher latency, handshake needed |
| UDP | Low-latency, loss OK | Minimal overhead | Packet loss, no ordering |
| NNG Pub/Sub | One-to-many reliable | Scales to multiple receivers | Requires NNG runtime |
| NNG Pair | Simple 1:1 | Bidirectional | One receiver only |
| WebRTC | Internet/NAT traversal | Encrypted, low-latency | Complex setup, beta |

Recommendations:

- Local testing: TCP loopback
- LAN production: TCP or NNG Pub/Sub
- Low-latency intra-LAN: UDP with acceptance of loss
- Internet: WebRTC when enabled; otherwise VPN + TCP/NNG
- One-to-many: NNG Pub/Sub or UDP multicast (future)

---

This guide completes M0.4 documentation for transport role pairing. Implementation work in future milestones should adhere to these pairings and URL formats, and expose necessary settings in the UI.
