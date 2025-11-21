# Open3DTransportQUIC - Complete Implementation Plan

**Issue Type:** Feature Implementation  
**Priority:** High  
**Estimated Complexity:** Large (3-4 weeks)  
**Dependencies:** msquic library (recommended), UE 5.7 build system  
**Target Platform:** Win64 (initially), architecturally ready for Linux/Mac

---

## Executive Summary

Implement a complete QUIC transport module (`Open3DTransportQUIC`) for the Open3DBroadcast plugin, following the established architectural patterns from existing transports (NNG, WebRTC, Sockets, Loopback). The implementation uses **MoQ (Media over QUIC) pub/sub semantics** to enable N:M deployments with named track subscription, mirroring NNG's flexible messaging patterns while leveraging QUIC's modern features: multiplexing, low latency, built-in encryption, and connection migration.

### Key Objectives

1. **Complete interface compliance** with `IOpen3DSender` and `IOpen3DReceiver`
2. **MoQ pub/sub semantics** for N:M deployments with track-based subscription
3. **Full audio support** using O3DAudio framework (PCM16/Opus)
4. **Editor UI customization** for host/port/track configuration
5. **Comprehensive testing** with UE automation tests (17+ tests)
6. **Production-ready documentation** (README, USER_GUIDE, IMPLEMENTATION_SUMMARY)
7. **Platform support**: Win64 first, Linux/Mac architectural support

---

## Table of Contents

1. [Prerequisites & Dependencies](#prerequisites--dependencies)
2. [Architecture Overview](#architecture-overview)
3. [Implementation Phases](#implementation-phases)
4. [Detailed Task Breakdown](#detailed-task-breakdown)
5. [File Structure](#file-structure)
6. [Testing Strategy](#testing-strategy)
7. [Documentation Requirements](#documentation-requirements)
8. [Risks & Mitigation](#risks--mitigation)
9. [Success Criteria](#success-criteria)
10. [Reference Material](#reference-material)

---

## Prerequisites & Dependencies

### QUIC Library Selection

**Recommended: msquic**
- **Repository:** https://github.com/microsoft/msquic
- **License:** MIT (compatible with Open3DStream)
- **Platforms:** Win64, Linux, Mac
- **Maturity:** Production-ready, used by Microsoft products
- **API:** C API, straightforward to wrap with C++
- **Features:** QUIC v1 (RFC 9000), multiplexing, 0-RTT, congestion control

**Alternatives Evaluated:**
- **quiche** (Cloudflare): Rust-based, requires FFI layer, more complex integration
- **ngtcp2**: C library, less mature than msquic, smaller ecosystem
- **Decision:** msquic wins on maturity, platform support, and API simplicity

### Build Dependencies

```
Required:
- Unreal Engine 5.7 (verified API compatibility)
- msquic 2.3+ (latest stable)
- OpenSSL (for TLS, may be bundled with msquic)
- CMake (for building msquic if needed)

UE Module Dependencies:
- Core, CoreUObject, Engine (standard)
- Open3DShared (O3D types, audio codec)
- Open3DSender (IOpen3DSender interface)
- Open3DReceiver (IOpen3DReceiver interface)
- Sockets, Networking (for address resolution)
- Slate, SlateCore, AppFramework (editor UI)
```

### Environment Variables

Add to O3DBuildFlags system (Open3DShared.Build.cs):
```csharp
bool WithQUIC = true; // Default enabled
Result.WithQUIC = ReadBool("O3D_WITH_TRANSPORT_QUIC", Result.WithQUIC);
```

---

## Architecture Overview

### Design Principles

1. **MoQ pub/sub semantics**: Named track subscription enabling N:M deployments (production mode)
2. **Dual deployment modes**: 
   - **Client-Server mode**: Direct QUIC connections for local testing and development
   - **Relay mode**: MoQ relay for production N:M pub/sub deployments
3. **Modular isolation**: Self-contained module with no changes to core Open3D* modules
4. **Interface compliance**: 100% parity with IOpen3DSender/IOpen3DReceiver contracts
5. **Threading model**: Async I/O with dedicated worker threads (following NNG pattern)
6. **Stream multiplexing**: Separate QUIC streams for control, mocap data, and audio
7. **Backpressure handling**: Queue-based buffering with overflow detection
8. **Connection management**: Auto-reconnect with exponential backoff

### MoQ Pub/Sub Architecture

The implementation follows **Media over QUIC (MoQ)** concepts for track-based pub/sub:

#### Deployment Modes

**1. Client-Server Mode (Local Testing)**
- Direct QUIC connection between sender and receiver
- Single publisher → single subscriber (1:1)
- Used for local network testing and development
- No relay required
- Simpler configuration and debugging

**2. Relay Mode (Production)**
- Publishers and subscribers connect to centralized MoQ relay
- Multiple publishers and subscribers (N:M)
- Track-based routing via relay
- **This is the production deployment model**
- Enables scalable, distributed deployments
- Future migration path to CloudFlare enterprise relay

#### MoQ Roles

**Publisher Role (Sender)**:
- Announces available tracks (e.g., "mocap/character1", "audio/character1")
- Publishes objects (individual frames) to subscribed tracks
- In relay mode: connects to relay and announces tracks
- In client-server mode: accepts direct subscriber connections

**Subscriber Role (Receiver)**:
- Discovers available tracks via control channel
- Subscribes to tracks by name pattern
- Receives objects for subscribed tracks only
- Supports multiple simultaneous subscriptions
- In relay mode: connects to relay and subscribes to tracks
- In client-server mode: connects directly to publisher

#### Track Naming Convention
```
<namespace>/<identifier>/<subtrack>
Examples:
  - "mocap/session1/character1"  (skeletal animation)
  - "audio/session1/character1"  (audio stream)
  - "mocap/session1/*"           (all characters in session - wildcard subscription)
```

### QUIC Stream Strategy with MoQ Semantics

**Stream 0: Control Channel (MoQ Control)**
- Bidirectional, reliable
- ANNOUNCE messages (publisher → subscriber)
- SUBSCRIBE/UNSUBSCRIBE messages (subscriber → publisher)
- Track metadata exchange
- Connection health monitoring

**Streams 1-N: Mocap Data Tracks (MoQ Objects)**
- Unidirectional, unreliable via QUIC datagrams (for low latency)
- Each track can use separate stream or shared datagram channel
- Object sequence numbers for loss detection
- Fallback to reliable ordered stream if datagram size exceeded
- Per-track priority levels

**Streams N+1 onwards: Audio Data Tracks**
- Unidirectional, reliable ordered
- PCM16 or Opus encoded frames
- Separate streams per audio track
- Higher priority than mocap to prevent starvation

### Component Diagram with MoQ Pub/Sub

```
┌─────────────────────────────────────────────────────────────────┐
│                    Open3DTransportQUIC Module                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌──────────────────┐              ┌──────────────────┐         │
│  │  FO3DQuicSender  │              │ FO3DQuicReceiver │         │
│  │  (Publisher)     │              │  (Subscriber)    │         │
│  │                  │              │                  │         │
│  │ - Initialize()   │              │ - Initialize()   │         │
│  │ - Start()        │              │ - Start()        │         │
│  │ - Stop()         │              │ - Stop()         │         │
│  │ - Send()         │◄────MoQ─────►│ - Poll()         │         │
│  │ - Tick()         │   Objects    │ - SetConsumer()  │         │
│  │ - CreateAudio    │              │ - SetAudioSink() │         │
│  │   Sink()         │              │                  │         │
│  └────────┬─────────┘              └────────┬─────────┘         │
│           │                                 │                    │
│           │    ┌────────────────────────────┘                    │
│           │    │                                                 │
│  ┌────────▼────▼─────────────────────────┐                      │
│  │    FMoQTrackManager                   │                      │
│  │    (MoQ Pub/Sub Layer)                │                      │
│  │                                        │                      │
│  │  - PublishTrack(name, priority)       │                      │
│  │  - SubscribeTrack(name, callback)     │                      │
│  │  - PublishObject(track, data, seq)    │                      │
│  │  - HandleControlMessages()            │                      │
│  │  - Track metadata management          │                      │
│  └────────────────┬───────────────────────┘                      │
│                   │                                              │
│         ┌─────────▼──────────┐                                  │
│         │  FQuicConnection   │                                  │
│         │  (QUIC Wrapper)    │                                  │
│         │                    │                                  │
│         │ - QUIC_HANDLE      │                                  │
│         │ - Stream Muxing    │                                  │
│         │ - Event Callbacks  │                                  │
│         │ - Datagram Support │                                  │
│         └─────────┬──────────┘                                  │
│                   │                                              │
│         ┌─────────▼──────────┐                                  │
│         │   msquic Library   │                                  │
│         │   (ThirdParty/)    │                                  │
│         └────────────────────┘                                  │
│                                                                   │
│  ┌──────────────────────────────────────────────────────┐       │
│  │           Editor UI Customization                     │       │
│  │  - SQuicSenderSettingsPanel (Slate widget)           │       │
│  │  - SQuicReceiverSettingsPanel (Slate widget)         │       │
│  └──────────────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────────────┘
```

### Lightweight MoQ Relay Architecture

The lightweight relay is an **interim solution** for N:M deployments with a clear migration path to CloudFlare's enterprise MoQ relay.

**Relay Responsibilities:**
1. **Track Directory**: Maintain mapping of track names to publisher connections
2. **Subscription Management**: Route SUBSCRIBE messages to appropriate publishers
3. **Object Forwarding**: Fan out objects from publishers to all subscribed receivers
4. **Connection Management**: Handle publisher and subscriber lifecycle
5. **Stats and Monitoring**: Track active tracks, subscriber counts, throughput

**Relay Deployment Diagram:**
```
┌──────────────┐         ┌──────────────┐         ┌──────────────┐
│  Publisher 1 │────────►│              │◄────────│ Subscriber 1 │
│ (UE Sender)  │ ANNOUNCE│  Lightweight │SUBSCRIBE│ (UE Receiver)│
└──────────────┘  +      │  MoQ Relay   │    +    └──────────────┘
                OBJECTS  │              │ OBJECTS
┌──────────────┐         │   (Server)   │         ┌──────────────┐
│  Publisher 2 │────────►│              │◄────────│ Subscriber 2 │
│ (UE Sender)  │         │  - Track Dir │         │ (UE Receiver)│
└──────────────┘         │  - Fanout    │         └──────────────┘
                         │  - Stats     │
                         └──────────────┘         ┌──────────────┐
                                  ▲               │ Subscriber 3 │
                                  └───────────────┤ (UE Receiver)│
                                                  └──────────────┘
```

**Migration to CloudFlare MoQ:**
- **Phase 1**: Use lightweight relay for initial production deployments
- **Phase 2**: Test CloudFlare MoQ relay compatibility
- **Phase 3**: Migrate to CloudFlare for enterprise-scale (100+ publishers/subscribers)
- **Key**: MoQ protocol compatibility ensures smooth migration

---

## Implementation Phases

### Phase 0: Setup & Dependencies (3-5 days)
**Goal:** Establish build infrastructure and third-party library integration

**Tasks:**
- [ ] 0.1: Build/acquire msquic binaries for Win64
- [ ] 0.2: Create module directory structure
- [ ] 0.3: Create Open3DTransportQUIC.Build.cs with msquic linkage
- [ ] 0.4: Add O3D_WITH_TRANSPORT_QUIC flag to O3DBuildFlags
- [ ] 0.5: Verify clean compile with empty module stub
- [ ] 0.6: Document msquic version and build configuration

### Phase 1: MoQ Protocol Layer (6-8 days)
**Goal:** Implement MoQ pub/sub semantics for track-based communication

**Tasks:**
- [ ] 1.1: Create MoQProtocol.h with message type definitions
- [ ] 1.2: Define MoQ control messages (ANNOUNCE, SUBSCRIBE, UNSUBSCRIBE, SUBSCRIBE_OK, SUBSCRIBE_ERROR)
- [ ] 1.3: Define MoQ object message format (track_id, sequence, priority, payload)
- [ ] 1.4: Implement control message serialization/deserialization
- [ ] 1.5: Implement object message serialization/deserialization
- [ ] 1.6: Create FMoQTrackManager class for track state management
- [ ] 1.7: Implement PublishTrack() - announce track to subscribers
- [ ] 1.8: Implement SubscribeTrack() - request subscription to track
- [ ] 1.9: Implement track name pattern matching (e.g., "mocap/session1/*")
- [ ] 1.10: Implement track priority and reliability mode settings
- [ ] 1.11: Add track subscription state machine (PENDING, ACTIVE, ERROR, CLOSED)

### Phase 2: Core Sender Implementation with MoQ Publisher (6-8 days)
**Goal:** Working sender that publishes tracks and sends objects to subscribers

**Tasks:**
- [ ] 2.1: Implement FQuicConnection wrapper class
- [ ] 2.2: Implement FO3DQuicSender skeleton with IOpen3DSender interface
- [ ] 2.3: Integrate FMoQTrackManager into sender
- [ ] 2.4: Implement connection establishment (server mode for publishers)
- [ ] 2.5: Implement control stream (Stream 0) for MoQ messages
- [ ] 2.6: Implement track announcement on connection (ANNOUNCE messages)
- [ ] 2.7: Implement SUBSCRIBE message handling - add client to track subscribers
- [ ] 2.8: Implement Send() method - publish object to mocap track
- [ ] 2.9: Implement async send worker thread (following NNG pattern)
- [ ] 2.10: Implement multi-subscriber fanout for published objects
- [ ] 2.11: Implement Tick() for track health monitoring and subscriber management
- [ ] 2.12: Implement Stop() with graceful UNANNOUNCE messages

### Phase 3: Core Receiver Implementation with MoQ Subscriber (5-7 days)
**Goal:** Working receiver that subscribes to tracks and receives objects

**Tasks:**
- [ ] 3.1: Implement FO3DQuicReceiver skeleton with IOpen3DReceiver interface
- [ ] 3.2: Integrate FMoQTrackManager into receiver
- [ ] 3.3: Implement connection establishment (client mode for subscribers)
- [ ] 3.4: Implement control stream handling for MoQ messages
- [ ] 3.5: Implement ANNOUNCE message handling - discover available tracks
- [ ] 3.6: Implement track subscription (SUBSCRIBE messages for configured tracks)
- [ ] 3.7: Implement SUBSCRIBE_OK/ERROR message handling
- [ ] 3.8: Implement OBJECT message reception and queuing
- [ ] 3.9: Implement Poll() method - dequeue objects from subscribed tracks
- [ ] 3.10: Implement automatic resubscription on reconnect
- [ ] 3.11: Implement latency tracking per track
- [ ] 3.12: Implement Stop() with UNSUBSCRIBE messages

### Phase 4: Audio Support with MoQ Audio Tracks (4-6 days)
**Goal:** Full audio transmission capability using separate MoQ audio tracks

**Tasks:**
- [ ] 4.1: Implement FO3DQuicSenderAudioSink
- [ ] 4.2: Implement CreateAudioSink() factory
- [ ] 4.3: Auto-announce audio track on CreateAudioSink() (e.g., "audio/session1/character1")
- [ ] 4.4: Publish audio objects to audio track via MoQTrackManager
- [ ] 4.5: Implement receiver audio track subscription
- [ ] 4.6: Implement receiver audio delivery via SetAudioSink()
- [ ] 4.7: Audio error handling and track state management

### Phase 5: Lightweight MoQ Relay Implementation (5-7 days)
**Goal:** Standalone relay for N:M deployments before CloudFlare enterprise relay

**Tasks:**
- [ ] 5.1: Create standalone MoQ relay project/module
- [ ] 5.2: Implement relay connection acceptance (both publishers and subscribers)
- [ ] 5.3: Implement track directory - map track names to publishers
- [ ] 5.4: Implement ANNOUNCE message forwarding - add track to directory
- [ ] 5.5: Implement SUBSCRIBE message routing - connect subscriber to publisher's track
- [ ] 5.6: Implement object forwarding - relay objects from publisher to subscribers
- [ ] 5.7: Implement multi-subscriber fanout for each track
- [ ] 5.8: Implement connection health monitoring and cleanup
- [ ] 5.9: Implement relay configuration (listen address, port, max connections)
- [ ] 5.10: Add relay stats and monitoring (active tracks, subscribers per track, throughput)
- [ ] 5.11: Document relay deployment and configuration
- [ ] 5.12: Test relay with multiple publishers and subscribers

### Phase 6: Configuration & Options with Track Names (3-4 days)
**Goal:** Flexible configuration parsing with MoQ track naming support

**Tasks:**
- [ ] 6.1: Define configuration keys in QuicHelpers.h (host, port, track_name, relay_mode)
- [ ] 6.2: Implement ParseSenderOptions() with track name parsing
- [ ] 6.3: Implement ParseReceiverOptions() with track name pattern support
- [ ] 6.4: Implement MakeStreamId() using track names
- [ ] 6.5: Add relay configuration options (relay_url, relay_port)

### Phase 7: Editor UI Customization with Track Selection (3-4 days)
**Goal:** User-friendly Slate widgets with track name configuration and mode selection

**Tasks:**
- [ ] 7.1: Implement SQuicSenderSettingsPanel with track name input
- [ ] 7.2: Implement SQuicReceiverSettingsPanel with track name pattern input
- [ ] 7.3: Add deployment mode selector (Client-Server for testing, Relay for production)
- [ ] 7.4: Add relay address configuration fields (only shown in relay mode)
- [ ] 7.5: Add warning/info text explaining mode usage (client-server = local testing, relay = production)
- [ ] 7.6: Register UI customizations in module startup
- [ ] 7.7: Test UI in Unreal Editor for both modes

### Phase 8: Module Registration & Integration (2-3 days)
**Goal:** Plugin discovers and uses QUIC transport automatically

**Tasks:**
- [ ] 8.1: Implement Open3DTransportQUICModule.cpp
- [ ] 8.2: Register transport factories
- [ ] 8.3: Register customizations
- [ ] 8.4: Update Open3DBroadcast.uplugin
- [ ] 8.5: Verify plugin loads

### Phase 9: Automated Testing with MoQ Scenarios (5-7 days)
**Goal:** Comprehensive test coverage including both client-server and relay modes

**Tasks:**
- [ ] 9.1: Create QuicTransportTests.cpp
- [ ] 9.2: Implement initialization tests (3 tests)
- [ ] 9.3: Implement client-server mode tests (4 tests - direct connection, 1:1 communication)
- [ ] 9.4: Implement MoQ track announcement tests (3 tests)
- [ ] 9.5: Implement MoQ track subscription tests (4 tests)
- [ ] 9.6: Implement data transfer tests in both modes (3 tests)
- [ ] 9.7: Implement multi-subscriber tests via relay (2 tests)
- [ ] 9.8: Implement audio track tests (3 tests)
- [ ] 9.9: Implement relay tests (4 tests - announce, subscribe, forward, N:M fanout)
- [ ] 9.10: Implement state management tests (2 tests)
- [ ] 9.11: Implement error/edge case tests (2 tests)
- [ ] 9.12: Platform-specific tests

### Phase 10: Documentation with MoQ & Relay (4-5 days)
**Goal:** Production-quality documentation covering both modes and relay deployment

**Tasks:**
- [ ] 10.1: Create README.md with MoQ overview and deployment modes
- [ ] 10.2: Create USER_GUIDE.md with:
  - Track naming conventions
  - Client-server mode setup (for local testing)
  - Relay mode setup (for production)
  - Mode selection guidelines
- [ ] 10.3: Create RELAY_DEPLOYMENT.md for lightweight relay:
  - Relay installation and configuration
  - Deployment architecture diagrams
  - Performance tuning
  - Monitoring and troubleshooting
- [ ] 10.4: Create IMPLEMENTATION_SUMMARY.md with MoQ architecture
- [ ] 10.5: Document migration path from lightweight relay to CloudFlare enterprise relay
- [ ] 10.6: Add clear guidance: client-server for testing, relay for production
- [ ] 10.7: Inline code documentation

### Phase 11: Build & Integration Testing (3-4 days)
**Goal:** Verify module and relay in both client-server and production scenarios

**Tasks:**
- [ ] 11.1: Clean build verification (module + relay)
- [ ] 11.2: Client-server mode testing (1:1, local network)
- [ ] 11.3: Relay mode testing (publishers and subscribers via relay)
- [ ] 11.4: N:M production scenario testing (2 publishers, 5 subscribers via relay)
- [ ] 11.5: Cross-transport interoperability
- [ ] 11.6: Audio end-to-end test in both modes
- [ ] 11.7: Performance benchmarking (client-server vs relay latency)
- [ ] 11.8: Relay stress testing (10+ connections, multi-track fanout)
- [ ] 11.9: Verify client-server mode suitable for local testing only

### Phase 12: Finalization & PR (2-3 days)
**Goal:** Code review, polish, and merge readiness

**Tasks:**
- [ ] 12.1: Code review preparation
- [ ] 12.2: Documentation review
- [ ] 12.3: Testing final pass
- [ ] 12.4: Changelog and version update
- [ ] 12.5: Pull request with MoQ and relay documentation

---

## File Structure

Complete file tree for Open3DTransportQUIC module with MoQ relay:

```
ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportQUIC/
├── Open3DTransportQUIC.Build.cs          [Build configuration, msquic linkage]
├── README.md                              [Module overview, deployment modes]
├── USER_GUIDE.md                          [Configuration guide, client-server vs relay]
├── RELAY_DEPLOYMENT.md                    [Relay setup and production deployment]
├── IMPLEMENTATION_SUMMARY.md              [MoQ architecture and design decisions]
│
├── Private/
│   ├── Open3DTransportQUICModule.cpp      [Module registration, transport factory]
│   │
│   ├── MoQ/
│   │   ├── MoQProtocol.h                  [MoQ message types and constants]
│   │   ├── MoQProtocol.cpp                [Message serialization/deserialization]
│   │   ├── MoQTrackManager.h              [Track announcement and subscription logic]
│   │   └── MoQTrackManager.cpp            [Track state management, routing]
│   │
│   ├── Sender/
│   │   ├── QuicSender.h                   [FO3DQuicSender - MoQ publisher]
│   │   ├── QuicSender.cpp                 [IOpen3DSender with track publishing]
│   │   ├── QuicSenderAudioSink.h          [FO3DQuicSenderAudioSink class]
│   │   └── QuicSenderAudioSink.cpp        [Audio track publishing]
│   │
│   ├── Receiver/
│   │   ├── QuicReceiver.h                 [FO3DQuicReceiver - MoQ subscriber]
│   │   └── QuicReceiver.cpp               [IOpen3DReceiver with track subscription]
│   │
│   ├── Shared/
│   │   ├── QuicHelpers.h                  [Configuration parsing, utilities]
│   │   ├── QuicHelpers.cpp                [ParseSenderOptions, ParseReceiverOptions]
│   │   ├── QuicConnection.h               [RAII wrapper for QUIC_HANDLE]
│   │   └── QuicConnection.cpp             [Connection lifecycle management]
│   │
│   └── Tests/
│       └── QuicTransportTests.cpp         [25+ automation tests (both modes)]
│
├── Relay/                                 [Lightweight MoQ relay - standalone]
│   ├── MoQRelay.h                         [Relay server class]
│   ├── MoQRelay.cpp                       [Track directory and forwarding logic]
│   ├── RelayConnection.h                  [Per-client connection management]
│   ├── RelayConnection.cpp                [Publisher/subscriber handling]
│   ├── RelayMain.cpp                      [Relay executable entry point]
│   ├── RelayConfig.h                      [Configuration structure]
│   ├── RelayConfig.cpp                    [Config file parsing]
│   └── README.md                          [Relay build and deployment instructions]
│
└── ThirdParty/
    ├── README.md                          [msquic version, build instructions]
    └── msquic/
        ├── include/
        │   └── msquic.h                   [msquic public API]
        ├── lib/
        │   └── Win64/
        │       ├── msquic.lib             [Import library]
        │       └── msquic.dll             [Runtime DLL]
        └── bin/
            └── Win64/
                └── msquic.dll             [Copy for packaging]
```

**Note on Deployment Modes:**
- **Client-Server**: Direct connections between sender/receiver (for local testing only)
- **Relay (Production)**: All clients connect to relay for N:M pub/sub (production deployment)

---

## Testing Strategy

### Test Coverage Requirements

**Total: 17 automation tests**

**Category 1: Initialization (3 tests)**
- Valid config succeeds
- Invalid host fails gracefully
- Missing port uses default

**Category 2: Connection (4 tests)**
- Local server/client connection
- Start idempotence
- Poll before Start returns zero
- Stop cleans up resources

**Category 3: Data Transfer (3 tests)**
- Single frame round-trip
- Multiple frames in order
- Large payload fallback to reliable

**Category 4: Audio (3 tests)**
- SupportsAudio returns true
- CreateAudioSink returns valid sink
- Audio transmission end-to-end

**Category 5: State & Stats (2 tests)**
- GetStats returns accurate counts
- Stop is idempotent

**Category 6: Error Handling (2 tests)**
- Connection lost triggers reconnect
- Non-Win64 fails gracefully

### Performance Benchmarks

| Metric | Target | Measurement Method |
|--------|--------|-------------------|
| **Latency (local)** | < 5ms RTT | Timestamp in payload |
| **Latency (LAN)** | < 15ms RTT | Same as local |
| **Throughput** | > 10 MB/s | Send 1000 frames, measure time |
| **CPU (idle)** | < 1% | UE profiler |
| **CPU (30 FPS mocap)** | < 5% | UE profiler |
| **CPU (+ audio)** | < 7% | UE profiler |
| **Memory overhead** | < 10 MB | Task manager |
| **Connection time** | < 500ms | Time from Start() to first frame |

---

## Documentation Requirements

### README.md Requirements

- Module overview and features
- Quick start (5-minute setup)
- Configuration parameters reference
- Performance characteristics
- Platform support matrix
- License information

### USER_GUIDE.md Requirements

- Quick start guide
- Server vs client mode explanation
- URI format documentation
- Audio configuration guide
- Troubleshooting section (10+ scenarios)
- FAQ (15+ questions)

### IMPLEMENTATION_SUMMARY.md Requirements

- Architecture decisions rationale
- QUIC library choice justification
- Stream multiplexing strategy
- Threading model explanation
- Test coverage summary
- Performance analysis
- Known limitations
- Future enhancements

---

## Risks & Mitigation

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| msquic build fails on Win64 | Medium | High | Use official prebuilt binaries, test early |
| TLS certificate complexity | High | Medium | Use test certs, document generation clearly |
| QUIC datagram size limits | Low | Medium | Fallback to reliable stream, make threshold configurable |
| Audio sync issues | Low | High | Use separate stream, reference WebRTC audio code |
| UE API changes in 5.7 | Low | High | Verify all APIs against UE 5.7 docs before use |
| Threading bugs | Medium | High | Follow NNG threading pattern exactly, extensive testing |

### Contingency Plans

**If msquic won't build:**
- Fallback 1: Use quiche (Cloudflare's library) with Rust FFI
- Fallback 2: Use ngtcp2 (less mature but simpler)
- Fallback 3: Defer QUIC transport, document as future work

**If TLS is too painful:**
- Option 1: Support QUIC without encryption (non-standard but doable)
- Option 2: Only support localhost (no TLS needed for loopback)
- Option 3: Vendor a self-signed cert generator tool

**If audio sync fails:**
- Ship without audio initially, add in follow-up PR
- Audio is optional (SupportsAudio() = false is valid)

---

## Success Criteria

### Minimum Viable Product (MVP)

- [ ] Module compiles cleanly with O3D_WITH_TRANSPORT_QUIC=1
- [ ] Sender and receiver implement all interface methods
- [ ] Local connection (sender as server, receiver as client) works
- [ ] Mocap data transmits reliably at 30 FPS for 5+ minutes
- [ ] Audio transmits (PCM16 minimum) without stuttering
- [ ] UI appears in editor for both sender and receiver
- [ ] At least 10 automation tests pass
- [ ] README, USER_GUIDE, IMPLEMENTATION_SUMMARY exist and accurate

### Production Ready (Goal)

- [ ] All 17 automation tests pass
- [ ] Performance meets targets (< 5ms latency local, < 5% CPU)
- [ ] Zero warnings on clean build
- [ ] Documentation complete with troubleshooting and FAQ
- [ ] Runs stable for 30+ minutes in editor
- [ ] Code review feedback addressed
- [ ] Benchmarks show QUIC is competitive with NNG/Sockets

### Stretch Goals (Nice-to-Have)

- [ ] Linux support (requires msquic Linux binaries)
- [ ] Mac support (requires msquic Mac binaries)
- [ ] 0-RTT connection resumption
- [ ] Congestion control tuning options
- [ ] Certificate generation helper tool
- [ ] Live latency graph in editor

---

## Reference Material

### Existing Transport Implementations

**Study these files closely for patterns:**

1. **Open3DTransportNNG** (most comprehensive)
   - File: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportNNG/`
   - `NngSender.h/.cpp` - Async worker thread pattern
   - `NngReceiver.h/.cpp` - Polling, reconnection logic
   - `NngHelpers.h/.cpp` - Configuration parsing
   - `Open3DTransportNNGModule.cpp` - Registration, UI customization
   - `NngTransportTests.cpp` - Test structure

2. **Open3DTransportWebRTC** (audio reference)
   - File: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/`
   - `WebRTCSender.cpp` - Audio sink implementation
   - `WebRTCReceiver.cpp` - Audio delivery
   - `WebRTCTransportTests.cpp` - Audio test cases
   - `USER_GUIDE.md` - Documentation style

3. **Open3DTransportSockets** (simpler example)
   - File: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportSockets/`
   - Minimal TCP/UDP implementation

### UE 5.7 API References

**Verify these APIs before use:**
- `FRunnableThread` - For async worker threads
- `TQueue<>` or `TCircularQueue<>` - For payload buffering
- `FCriticalSection` / `FScopeLock` - For thread safety
- `FDateTime::Now()` - For timestamp/latency tracking
- `FPlatformProcess::Sleep()` - For backoff/delays
- `TSharedPtr<>` / `MakeShared<>` - For object lifetime

**Reference:** https://dev.epicgames.com/documentation/en-us/unreal-engine/API

### msquic Documentation

- GitHub: https://github.com/microsoft/msquic
- API Reference: https://microsoft.github.io/msquic/
- Samples: https://github.com/microsoft/msquic/tree/main/src/tools/sample

### O3DS Protocol

- FlatBuffers Schema: `src/o3ds.fbs`
- Generated headers: `src/o3ds_generated.h`
- Serialization: `O3DS::SubjectList::Serialize()`
- Audio encoding: `O3DAudio::FFrameEncoder`
- Audio decoding: `O3DAudio::FFrameDecoder`

---

## Appendix: QUIC vs Other Transports

| Feature | TCP (Sockets) | UDP (Sockets) | NNG | QUIC | WebRTC |
|---------|---------------|---------------|-----|------|--------|
| **Connection-oriented** | ✅ | ❌ | ✅ (Pair) | ✅ | ✅ |
| **Reliable delivery** | ✅ | ❌ | ✅ | ✅ | ✅ |
| **Unreliable mode** | ❌ | ✅ | ❌ | ✅ (datagrams) | ✅ (data channel) |
| **Multiplexing** | ❌ | ❌ | ❌ | ✅ (streams) | ✅ (channels) |
| **Built-in encryption** | ❌ | ❌ | ❌ | ✅ (TLS 1.3) | ✅ (DTLS) |
| **NAT traversal** | ❌ | ❌ | ❌ | ⚠️ (possible) | ✅ (STUN/TURN) |
| **Low latency** | ⚠️ | ✅ | ⚠️ | ✅ | ⚠️ (signaling) |
| **Setup complexity** | Low | Low | Medium | Medium | High |
| **Server required** | ❌ | ❌ | ❌ | ❌ | ✅ (LiveKit) |

**Use QUIC when:**
- You need both reliable and unreliable modes
- You want built-in encryption without WebRTC complexity
- You need stream multiplexing (mocap + audio on one connection)
- You want modern congestion control (better than TCP)

**Don't use QUIC when:**
- You need simple localhost testing (use Loopback)
- You need advanced messaging patterns like pub/sub (use NNG)
- You need guaranteed NAT traversal (use WebRTC)

---

## Estimated Timeline

**Total Duration:** 3-4 weeks (60-80 hours)

### Week 1: Core Implementation
- Days 1-2: Setup & dependencies (Phase 0)
- Days 3-5: Core sender (Phase 1)

### Week 2: Receiver & Audio
- Days 1-2: Core receiver (Phase 2)
- Days 3-5: Audio support (Phase 3)

### Week 3: Polish & Testing
- Days 1-2: Configuration & UI (Phase 4, 5)
- Days 3-5: Automated tests (Phase 7)

### Week 4: Documentation & QA
- Days 1-2: Documentation (Phase 8)
- Days 3-4: Integration testing (Phase 9)
- Day 5: PR preparation (Phase 10)

**Contingency:** +1 week buffer for unforeseen issues

---

**Plan Status:** READY FOR IMPLEMENTATION  
**Plan Version:** 1.0  
**Last Updated:** 2025-11-21  
**Created By:** Planning Agent  
**Review Status:** Ready for Coding Agent

---

**NOTE FOR CODING AGENT:**

This plan is structured to be executed step-by-step. Each phase builds on the previous one. Do not skip phases or combine them unless explicitly noted as parallelizable.

**Before starting:**
1. Read this entire document
2. Study the existing NNG and WebRTC transport implementations
3. Verify UE 5.7 API compatibility for all planned APIs
4. Acquire msquic binaries or confirm build process

**During implementation:**
1. Follow phases in order (0 → 10)
2. Test each phase before moving to the next
3. Document architectural decisions as you go
4. Update this plan if you discover issues
5. Ask for clarification if blocked > 4 hours

**Key principles:**
- Small steps > big leaps
- Test early, test often
- Never guess UE APIs - always verify
- Reference existing transports when uncertain
- Document tradeoffs

Good luck! 🚀
