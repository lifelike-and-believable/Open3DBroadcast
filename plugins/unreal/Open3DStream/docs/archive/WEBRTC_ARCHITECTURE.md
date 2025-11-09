# WebRTC Architecture Diagram

## System Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Open3DStream WebRTC Flow                      │
└─────────────────────────────────────────────────────────────────┘

┌──────────────┐                                      ┌──────────────┐
│   Sender     │                                      │   Receiver   │
│  (Motion     │                                      │  (Unreal     │
│   Capture)   │                                      │   Engine)    │
└──────┬───────┘                                      └──────┬───────┘
       │                                                     │
       │ O3DS::WebRTCClient                 O3DS::WebRTCClient│
       │ webrtc://signal:8080/room          webrtc://signal:8080/room
       │                                                     │
       │                                                     │
       ▼                                                     ▼
┌──────────────────────────────────────────────────────────────────┐
│                      Signaling Server                             │
│                   (WebSocket on port 8080)                        │
│                                                                   │
│  ┌────────────┐  Room: "room"  ┌────────────┐                   │
│  │  Sender    │◄──────────────►│  Receiver  │                   │
│  │  Peer A    │                │  Peer B    │                   │
│  └────────────┘                └────────────┘                   │
│                                                                   │
│  Message Flow:                                                    │
│  1. JOIN room                                                     │
│  2. OFFER (SDP)        ──────►                                   │
│  3.                            ◄────── ANSWER (SDP)              │
│  4. CANDIDATE (ICE)    ──────►                                   │
│  5.                            ◄────── CANDIDATE (ICE)           │
└───────────────────────────────────────────────────────────────────┘
                              │
                              │ ICE negotiation complete
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│                    STUN/TURN Servers                              │
│                  (NAT Traversal Helpers)                          │
│                                                                   │
│  ┌────────────────┐          ┌────────────────┐                 │
│  │  STUN Server   │          │  TURN Server   │                 │
│  │ (Google STUN)  │          │  (if needed)   │                 │
│  │  Port 19302    │          │  Port 3478     │                 │
│  └────────────────┘          └────────────────┘                 │
└───────────────────────────────────────────────────────────────────┘
                              │
                              │ Direct P2P or TURN relay
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│                WebRTC Data Channel Established                    │
│                      (DTLS Encrypted)                             │
│                                                                   │
│  ┌────────────┐      Binary O3DS Frames      ┌────────────┐     │
│  │  Sender    │ ──────────────────────────► │  Receiver  │     │
│  │            │                              │            │     │
│  │  Skeleton  │ ◄────── (optional ack) ───  │  LiveLink  │     │
│  │  + Curves  │                              │   Frames   │     │
│  └────────────┘                              └────────────┘     │
└───────────────────────────────────────────────────────────────────┘
```

## Connection States

```
┌────────────┐
│ NOTSTARTED │
└──────┬─────┘
       │ start("webrtc://...")
       ▼
┌────────────┐
│  STARTED   │──── Connect to signaling server
└──────┬─────┘
       │ WebSocket connected
       ▼
┌────────────┐
│  READING   │──── Create PeerConnection
└──────┬─────┘      Generate SDP offer
       │            Exchange ICE candidates
       ▼
┌────────────┐
│ CONNECTED  │──── Data channel open
└──────┬─────┘      Ready to send/receive
       │
       ▼
┌────────────┐
│   CLOSED   │──── Connection terminated
└────────────┘
```

## Data Flow

```
Motion Capture System
        │
        ▼
┌───────────────┐
│ O3DS Subject  │
│  Transforms   │
│    Curves     │
└───────┬───────┘
        │
        ▼
┌───────────────┐
│  Serialize    │
│ (FlatBuffers) │
└───────┬───────┘
        │
        ▼
┌───────────────┐
│ WebRTC Client │
│ write(data)   │
└───────┬───────┘
        │
        ▼
┌───────────────┐
│  Data Channel │
│    (Binary)   │
└───────┬───────┘
        │
        │ Network (UDP/TURN)
        │
        ▼
┌───────────────┐
│  Data Channel │
│   (Receiver)  │
└───────┬───────┘
        │
        ▼
┌───────────────┐
│ OnData        │
│  Callback     │
└───────┬───────┘
        │
        ▼
┌───────────────┐
│ Parse Subject │
│    List       │
└───────┬───────┘
        │
        ▼
┌───────────────────────┐
│ LiveLink Frame Data   │
│  - Transforms[]       │
│  - CurveNames[]       │
│  - CurveValues[]      │
└───────────────────────┘
        │
        ▼
Unreal Engine Character
```

## Protocol Comparison

```
TCP
════════════════════════════════════════════
[Sender] ──► [Direct Connection] ──► [Receiver]
         tcp://ip:port
Pros: Simple, reliable
Cons: No NAT traversal, server setup required


UDP
════════════════════════════════════════════
[Sender] ──► [Broadcast/Direct] ──► [Receiver]
         udp://ip:port
Pros: Low latency, simple
Cons: No NAT traversal, lossy


WebRTC
════════════════════════════════════════════
                ┌──────────────┐
                │  Signaling   │
                │    Server    │
                └──────┬───────┘
                       │
      ┌────────────────┼────────────────┐
      │                │                │
      ▼                ▼                ▼
[Sender] ◄──[STUN/TURN]──► [Receiver]
       webrtc://signal:port/room

Pros: NAT traversal, encrypted, P2P
Cons: Requires signaling server, complex setup
```

## NAT Traversal Scenarios

```
Scenario 1: Direct Connection (Best Case)
══════════════════════════════════════════
[Sender]                    [Receiver]
  LAN                         LAN
   │                           │
   └───── Direct UDP ─────────┘
   
Latency: 10-50ms
Quality: Excellent


Scenario 2: STUN (Common Case)
══════════════════════════════════════════
[Sender]           [STUN]         [Receiver]
  NAT               Server            NAT
   │                  │                │
   │──discover IP────►│                │
   │                  │◄────discover──│
   │                  │                │
   └──── Hole-punched UDP ───────────►│

Latency: 50-100ms
Quality: Good


Scenario 3: TURN Relay (Worst Case)
══════════════════════════════════════════
[Sender]           [TURN]         [Receiver]
Symmetric           Relay          Symmetric
   NAT             Server             NAT
   │                  │                │
   └─────► relay ────►│                │
                      │◄────relay ─────┘

Latency: 100-200ms
Quality: Fair (uses bandwidth)
```

## Message Sequence

```
Time
 │
 │  Sender                Signaling              Receiver
 │    │                      │                      │
 ├────┼─ JOIN room ─────────►│                      │
 │    │                      │◄──── JOIN room ──────┤
 │    │                      │                      │
 ├────┼─ OFFER (SDP) ───────►│                      │
 │    │                      ├─ OFFER (SDP) ───────►│
 │    │                      │                      │
 │    │                      │◄──── ANSWER (SDP) ───┤
 ├────┼◄─ ANSWER (SDP) ──────┤                      │
 │    │                      │                      │
 ├────┼─ ICE candidate ─────►│                      │
 │    │                      ├─ ICE candidate ─────►│
 │    │                      │                      │
 │    │                      │◄──── ICE candidate ──┤
 ├────┼◄─ ICE candidate ─────┤                      │
 │    │                      │                      │
 │    │         ... ICE negotiation ...             │
 │    │                      │                      │
 ├────┼─────── Data Channel Established ───────────►│
 │    │                      │                      │
 ├────┼═══════ O3DS Binary Data Stream ════════════►│
 │    │                      │                      │
 ▼    ▼                      ▼                      ▼
```

## Class Hierarchy

```
┌─────────────────┐
│   Connector     │ (base)
│   - mState      │
│   - mError      │
└────────┬────────┘
         │
         ▼
┌─────────────────┐
│ AsyncConnector  │
│  - mInDataFunc  │
│  - mContext     │
└────────┬────────┘
         │
         ├──────────────────┬──────────────────┬──────────────
         │                  │                  │
         ▼                  ▼                  ▼
┌─────────────────┐ ┌──────────────┐ ┌──────────────────┐
│  WebRTCClient   │ │ TCPConnector │ │ UDPConnector     │
├─────────────────┤ └──────────────┘ └──────────────────┘
│ - mPeerConn     │
│ - mDataChannel  │
│ - mSignalSocket │
└─────────────────┘
```

---

*This diagram provides a visual reference for understanding how WebRTC integrates
with the Open3DStream architecture. For implementation details, see
WEBRTC_IMPLEMENTATION_SUMMARY.md*
