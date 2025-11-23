# Open3DTransportMoQ - Complete Implementation Plan

**Issue Type:** Feature Implementation  
**Priority:** High  
**Estimated Complexity:** Large (4-5 weeks)  
**Dependencies:** moq-rs (Rust library via FFI), moq-relay-ietf, UE 5.7 build system  
**Target Platform:** Win64 (initially), architecturally ready for Linux/Mac  
**Last Updated:** 2025-11-21 (Updated for moq-rs integration)

---

## Change Summary (moq-rs Integration Update)

**Date:** 2025-11-21  
**Context:** Currently at Phase 2 of original plan  
**Change:** Integrate Cloudflare's moq-rs implementation instead of custom MoQ protocol

### Key Changes from Original Plan

**1. Protocol Implementation:**
- **Was:** Custom MoQ-like protocol built from scratch
- **Now:** IETF draft-ietf-moq-transport-07 via moq-rs library

**2. Technology Stack:**
- **Was:** msquic (C library) for QUIC
- **Now:** moq-transport (Rust) which uses quinn for QUIC, bridged via FFI

**3. Relay Architecture:**
- **Was:** Build custom "lightweight relay" in Phase 5
- **Now:** Use moq-relay-ietf from moq-rs (production-ready)

**4. Deployment Modes:**
- **Was:** Client-server mode (direct connections) + Relay mode
- **Now:** Relay-only mode (local relay or CloudFlare relay)

**5. Integration Approach:**
- **Was:** Direct C++ implementation
- **Now:** Rust (moq-transport) ↔ C (FFI) ↔ C++ (Unreal wrapper)

**6. Third-Party Delivery:**
- **Was:** Build FFI bindings inside this repo
- **Now:** Consume prebuilt wrappers from `/lifelike-and-believable/moq-ffi` (vendored under `ThirdParty/moq-ffi`)

**7. Implementation Baseline:**
- **Was:** Incrementally refactor the prototype MoQ transport
- **Now:** Restart Open3DTransportMoQ as a greenfield implementation on top of moq-ffi

### Benefits of This Approach

✅ **Standards Compliance:** Implements actual IETF MoQ specification  
✅ **Interoperability:** Works with other MoQ clients/servers  
✅ **Production Ready:** moq-rs is battle-tested by CloudFlare  
✅ **CloudFlare Network:** Direct access to experimental relay network  
✅ **Reduced Maintenance:** Protocol implementation maintained by CloudFlare  
✅ **Future-Proof:** Tracks IETF standardization process  

### Trade-offs

⚠️ **Additional Complexity:** FFI layer adds development and debugging complexity  
⚠️ **Build Dependencies:** Requires Rust toolchain  
⚠️ **Timeline Impact:** +2 weeks for FFI development and integration  
⚠️ **Learning Curve:** Team needs Rust FFI knowledge for maintenance  

### Migration Impact (Phases 0-2)

**Current Baseline:**
- `lifelike-and-believable/moq-ffi` already encapsulates the Rust↔C++ bridge and is mirrored under `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportMoQ/ThirdParty/moq-ffi`
- Previous MoQ prototype files remain for historical reference only and will not be refactored

**New Direction:**
- Treat Open3DTransportMoQ as a fresh implementation built directly on top of moq-ffi
- Use Phase 0 to wire the vendored package into the build, version it, and document refresh workflows
- Use Phase 1 to create the Unreal-facing wrappers/threading helpers that adapt moq-ffi to the transport interfaces

---

## Executive Summary

Implement a complete MoQ transport module (`Open3DTransportMoQ`) for the Open3DBroadcast plugin, following the established architectural patterns from existing transports (NNG, WebRTC, Sockets, Loopback). The implementation integrates with **Cloudflare's moq-rs implementation** to provide standards-compliant **MoQ (Media over QUIC) pub/sub semantics** for N:M deployments with named track subscription. This approach leverages the production-ready moq-transport library (implementing IETF draft-ietf-moq-transport-07) and moq-relay-ietf for relay functionality, enabling both local testing and internet-scale deployments via CloudFlare's experimental MoQ relay network.

### Key Objectives

1. **Complete interface compliance** with `IOpen3DSender` and `IOpen3DReceiver`
2. **moq-rs integration** via FFI for standards-compliant IETF MoQ protocol (draft-07)
3. **MoQ pub/sub semantics** for N:M deployments with track-based subscription
4. **Full audio support** using O3DAudio framework (PCM16/Opus)
5. **moq-relay-ietf deployment** for local relay and CloudFlare relay for internet-scale testing
6. **Editor UI customization** for relay URL/port/track configuration
7. **Comprehensive testing** with UE automation tests (20+ tests including relay scenarios)
8. **Production-ready documentation** (README, USER_GUIDE, RELAY_DEPLOYMENT, IMPLEMENTATION_SUMMARY)
9. **Platform support**: Win64 first, Linux/Mac architectural support

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

### MoQ Implementation: moq-rs Integration

**Primary Choice: moq-rs (Cloudflare)**
- **Repository:** https://github.com/cloudflare/moq-rs (branch: draft-ietf-moq-transport-07)
- **License:** MIT OR Apache-2.0 (compatible with Open3DStream)
- **Language:** Rust (requires FFI bridge to C++/Unreal)
- **Platforms:** Win64, Linux, Mac
- **Maturity:** Production-ready, implements IETF draft-ietf-moq-transport-07
- **Components:**
  - **moq-transport**: Core MoQ protocol library
  - **moq-relay-ietf**: Production relay server (standalone executable)
  - **moq-pub/moq-sub**: Reference client implementations
- **Transport Layer:** Uses quinn (Rust QUIC implementation) and WebTransport
- **Features:** Standards-compliant MoQ pub/sub, track announcements, subscriptions, object delivery

**Integration Strategy:**
1. Vendor and version the `lifelike-and-believable/moq-ffi` package (headers, libs, DLLs) inside `ThirdParty/moq-ffi`
2. Link the existing C++ wrappers from Unreal via `Open3DTransportMoQ.Build.cs` and extend `MoQFfiSupport` for runtime loading/validation
3. Implement sender/receiver/audio features purely on top of the provided wrappers (no new Rust code in-tree)
4. Use moq-relay-ietf as-is for relay server (local + CloudFlare tiers)
5. Deploy moq-relay-ietf locally for testing and validate against CloudFlare's experimental relay network

**Why moq-rs over custom implementation:**
- Standards compliance with IETF draft-ietf-moq-transport-07
- Production-tested implementation used in CloudFlare infrastructure
- Active development and community support
- Interoperability with other MoQ clients/servers
- Reduces implementation risk and maintenance burden
- Direct path to CloudFlare relay network for large-scale testing

### Third-Party FFI Package: moq-ffi (vendored)

- **Repository:** https://github.com/lifelike-and-believable/moq-ffi
- **Purpose:** Ships the Rust async runtime, C bindings, and C++ convenience wrappers that talk to moq-transport
- **Vendored Path:** `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportMoQ/ThirdParty/moq-ffi`
- **Contents:**
  - `include/moq_ffi.h` – C API surface exposed by the Rust crate
  - `lib/bin/Win64/Release` – Prebuilt `moq_ffi.lib` + `moq_ffi.dll` + PDB for Win64
  - `README.md` (to be added) – records upstream commit, build switches, and refresh instructions
- **Update Policy:** Matches the LiveKit FFI workflow (download artifact, drop into ThirdParty/, record commit + hash, never rebuild during normal UE builds)

### Build Dependencies

```
Required:
- Unreal Engine 5.7 (verified API compatibility)
- moq-ffi third-party package (vendored copy of https://github.com/lifelike-and-believable/moq-ffi)
  * `ThirdParty/moq-ffi/include` for headers
  * `ThirdParty/moq-ffi/lib` + `bin` for Win64 Release artifacts (Linux/Mac placeholders reserved)
- C++17 compiler (for transport module + thin wrappers)
- moq-relay-ietf binary (compiled from moq-rs) for runtime validation

UE Module Dependencies:
- Core, CoreUObject, Engine (standard)
- Open3DShared (O3D types, audio codec)
- Open3DSender (IOpen3DSender interface)
- Open3DReceiver (IOpen3DReceiver interface)
- Sockets, Networking (for address resolution)
- Slate, SlateCore, AppFramework (editor UI)

Optional / Refresh Workflows:
- Rust toolchain (rustc 1.70+, cargo) when rebuilding moq-ffi or moq-relay-ietf
- moq-rs repository (draft-ietf-moq-transport-07 branch)
- cbindgen or equivalent tooling used within the moq-ffi repo to regenerate headers
```

### Environment Variables

Add to O3DBuildFlags system (Open3DShared.Build.cs):
```csharp
bool WithMoQ = true; // Default enabled
Result.WithMoQ = ReadBool("O3D_WITH_TRANSPORT_MOQ", Result.WithMoQ);
```

### Relay Deployment Options

**Option 1: Local moq-relay-ietf (Development/Testing)**
- Deploy moq-relay-ietf from moq-rs on localhost or LAN server
- Full control over configuration and debugging
- Use for development, CI/CD, and local network testing

**Option 2: CloudFlare Experimental MoQ Relay (Internet-Scale Testing)**
- Connect to CloudFlare's hosted relay network at https://relay.cloudflare.mediaoverquic.com
- Test real-world internet conditions and latency
- Validate interoperability with other MoQ clients
- Use for performance benchmarking and scalability testing

---

## Architecture Overview

### Design Principles

1. **Standards-compliant MoQ**: Use moq-rs for IETF draft-ietf-moq-transport-07 compliance
2. **FFI architecture**: Rust (moq-transport) ↔ C (FFI layer) ↔ C++ (Unreal wrapper)
3. **Relay-based deployment**: All production traffic goes through moq-relay-ietf
4. **Modular isolation**: Self-contained module with no changes to core Open3D* modules
5. **Interface compliance**: 100% parity with IOpen3DSender/IOpen3DReceiver contracts
6. **Threading model**: Async I/O bridging Rust async/await to Unreal threading
7. **Track-based multiplexing**: MoQ tracks for mocap data, audio, and metadata
8. **Backpressure handling**: Queue-based buffering with overflow detection
9. **Connection management**: Leverage moq-transport's built-in connection handling

### MoQ Pub/Sub Architecture with moq-rs

The implementation uses **Cloudflare's moq-rs** for standards-compliant IETF MoQ protocol:

#### Deployment Architecture

**All traffic flows through moq-relay-ietf** - there is no "direct client-server mode" in this design:

**1. Local Relay Mode (Development/Testing)**
- Publishers and subscribers connect to locally deployed moq-relay-ietf
- Multiple publishers and subscribers (N:M)
- Track-based routing via local relay
- Full control for debugging and testing
- Use for development, CI/CD, and LAN testing
- Relay URL: `https://localhost:4443` or LAN server

**2. CloudFlare Relay Mode (Internet-Scale Testing)**
- Publishers and subscribers connect to CloudFlare's hosted MoQ relay
- Internet-scale, multi-region deployment
- Track-based routing via CloudFlare relay network
- **This is the production/internet-scale deployment model**
- Use for real-world performance testing and validation
- Enables truly distributed, global deployments
- Relay URL: https://relay.cloudflare.mediaoverquic.com

#### MoQ Roles (via moq-transport)

**Publisher Role (Sender)**:
- Connects to moq-relay-ietf via WebTransport
- Announces available tracks (e.g., "mocap/character1", "audio/character1")
- Publishes objects (individual frames) to announced tracks
- Uses moq-transport's `Session` API in publisher role
- Relay handles fan-out to all subscribers

**Subscriber Role (Receiver)**:
- Connects to moq-relay-ietf via WebTransport
- Discovers available tracks via relay's track catalog
- Subscribes to tracks by namespace pattern
- Receives objects for subscribed tracks only
- Supports multiple simultaneous subscriptions
- Uses moq-transport's `Session` API in subscriber role
- Relay handles track discovery and routing

#### Track Naming Convention
```
<namespace>/<identifier>/<subtrack>
Examples:
  - "mocap/session1/character1"  (skeletal animation)
  - "audio/session1/character1"  (audio stream)
  - "mocap/session1/*"           (all characters in session - wildcard subscription)
```

### MoQ Track Strategy (moq-transport handles streams)

**Managed by moq-transport library** - stream multiplexing is abstracted:

**Track Types:**

**1. Mocap Data Tracks**
- Track namespace: `"mocap/<session>/<character>"`
- Object per frame: each animation frame is a separate MoQ object
- Priority: configurable per track (default: medium)
- Delivery: depends on track properties (can be unreliable for low latency)
- Sequence numbers provided by moq-transport
- Example: `"mocap/session1/alice"`

**2. Audio Data Tracks**
- Track namespace: `"audio/<session>/<character>"`
- Object per audio chunk: each audio frame is a separate MoQ object
- Priority: configurable per track (default: high to prevent starvation)
- Delivery: typically reliable to avoid audio glitches
- Separate track per audio source
- Example: `"audio/session1/alice"`

**3. Metadata/Control Tracks (Optional)**
- Track namespace: `"meta/<session>/<character>"`
- For character metadata, scene state, etc.
- Priority: high for time-sensitive metadata
- Example: `"meta/session1/scene"`

**Note:** moq-transport handles all QUIC stream management, including:
- Control streams for ANNOUNCE/SUBSCRIBE messages
- Data streams for object delivery
- Stream multiplexing and prioritization
- Backpressure and flow control

### Component Diagram with moq-rs Integration

```
┌─────────────────────────────────────────────────────────────────────┐
│                     Open3DTransportMoQ Module                       │
├─────────────────────────────────────────────────────────────────────┤
│                          C++/Unreal Layer                            │
│  ┌──────────────────┐                  ┌──────────────────┐         │
│  │  FO3DMoQSender  │                  │ FO3DMoQReceiver │         │
│  │  (Publisher)     │                  │  (Subscriber)    │         │
│  │                  │                  │                  │         │
│  │ - Initialize()   │                  │ - Initialize()   │         │
│  │ - Start()        │    MoQ Objects   │ - Start()        │         │
│  │ - Stop()         │   via Relay      │ - Stop()         │         │
│  │ - Send()         │◄────────────────►│ - Poll()         │         │
│  │ - Tick()         │                  │ - SetConsumer()  │         │
│  │ - CreateAudio    │                  │ - SetAudioSink() │         │
│  │   Sink()         │                  │                  │         │
│  └────────┬─────────┘                  └────────┬─────────┘         │
│           │                                     │                    │
│  ┌────────▼─────────────────────────────────────▼──────────┐        │
│  │         FMoQSessionWrapper (C++ Wrapper)               │        │
│  │  - Wraps Rust moq_transport::Session                    │        │
│  │  - Manages WebTransport connection to relay             │        │
│  │  - Announces/subscribes to tracks                       │        │
│  │  - Publishes/receives objects                           │        │
│  │  - Bridges Rust async/await to Unreal threading        │        │
│  └────────────────────────┬────────────────────────────────┘        │
├───────────────────────────┼─────────────────────────────────────────┤
│                      FFI Boundary (C API)                            │
│  ┌────────────────────────▼────────────────────────────────┐        │
│  │     moq_ffi.h / moq_ffi.c (C bindings)                  │        │
│  │  - C-compatible function exports from Rust              │        │
│  │  - Handle opaque pointers to Rust objects               │        │
│  │  - Memory management across language boundary           │        │
│  │  - Error code translation                               │        │
│  └────────────────────────┬────────────────────────────────┘        │
├───────────────────────────┼─────────────────────────────────────────┤
│                         Rust Layer                                   │
│  ┌────────────────────────▼────────────────────────────────┐        │
│  │      moq-transport (Cloudflare moq-rs)                  │        │
│  │  - MoQ protocol implementation (draft-07)               │        │
│  │  - Session management (publisher/subscriber roles)      │        │
│  │  - Track announcement/subscription                      │        │
│  │  - Object serialization/deserialization                 │        │
│  │  - WebTransport client                                  │        │
│  └────────────────────────┬────────────────────────────────┘        │
│                           │                                          │
│  ┌────────────────────────▼────────────────────────────────┐        │
│  │      quinn (Rust QUIC implementation)                   │        │
│  │  - QUIC v1 (RFC 9000)                                   │        │
│  │  - Stream multiplexing                                  │        │
│  │  - TLS 1.3 encryption                                   │        │
│  │  - Connection management                                │        │
│  └─────────────────────────────────────────────────────────┘        │
│                                                                       │
│  ┌──────────────────────────────────────────────────────┐           │
│  │           Editor UI Customization                     │           │
│  │  - SMoQSenderSettingsPanel (Slate widget)           │           │
│  │    * Relay URL configuration                         │           │
│  │    * Track name/namespace input                      │           │
│  │  - SMoQReceiverSettingsPanel (Slate widget)         │           │
│  │    * Relay URL configuration                         │           │
│  │    * Track pattern subscription                      │           │
│  └──────────────────────────────────────────────────────┘           │
└─────────────────────────────────────────────────────────────────────┘

External:
┌────────────────────────────────────────────────────────────────┐
│  moq-relay-ietf (Standalone Rust Executable)                   │
│  - Track directory and routing                                 │
│  - Publisher/subscriber connection management                  │
│  - Object fan-out to subscribers                               │
│  - Deployed separately (local or CloudFlare hosted)            │
└────────────────────────────────────────────────────────────────┘
```

### moq-relay-ietf Deployment Architecture

The implementation uses **moq-relay-ietf** from the moq-rs repository - a production-ready relay implementing IETF MoQ draft-07.

**Relay Features (built-in to moq-relay-ietf):**
1. **Track Directory**: Maintains mapping of track names to publisher connections
2. **Subscription Management**: Routes SUBSCRIBE messages to appropriate publishers
3. **Object Forwarding**: Fans out objects from publishers to all subscribed receivers
4. **Connection Management**: Handles publisher and subscriber lifecycle via WebTransport
5. **Stats and Monitoring**: Tracks active tracks, subscriber counts, throughput
6. **Standards Compliance**: Implements IETF draft-ietf-moq-transport-07 specification

**Relay Deployment Diagram:**
```
┌──────────────┐         ┌──────────────────┐         ┌──────────────┐
│  Publisher 1 │────────►│                  │◄────────│ Subscriber 1 │
│ (UE Sender)  │ ANNOUNCE│  moq-relay-ietf  │SUBSCRIBE│ (UE Receiver)│
└──────────────┘  +      │                  │    +    └──────────────┘
                OBJECTS  │  (from moq-rs)   │ OBJECTS
┌──────────────┐         │                  │         ┌──────────────┐
│  Publisher 2 │────────►│  WebTransport    │◄────────│ Subscriber 2 │
│ (UE Sender)  │         │  QUIC/TLS 1.3    │         │ (UE Receiver)│
└──────────────┘         │                  │         └──────────────┘
                         │  - Track routing │
                         │  - N:M fan-out   │         ┌──────────────┐
                         │  - Standards     │         │ Subscriber 3 │
                         │    compliant     │◄────────┤ (UE Receiver)│
                         └──────────────────┘         └──────────────┘
                                  ▲
                                  │
                    Can be local or CloudFlare hosted
```

**Deployment Options:**

**Option 1: Local Relay (Development/Testing)**
- Build and deploy moq-relay-ietf from moq-rs repository
- Run on localhost or LAN server
- Full control for debugging and configuration
- Use for development, CI/CD, and local network testing
- Example: `moq-relay-ietf --bind [::]:4443 --tls-cert cert.pem --tls-key key.pem`

**Option 2: CloudFlare Relay (Internet-Scale)**
- Connect to CloudFlare's experimental MoQ relay network
- Production-grade infrastructure
- Multi-region, low-latency routing
- Use for real-world performance testing and large-scale deployments
- URL: `https://relay.cloudflare.mediaoverquic.com`

**Migration Path:**
- Start with local relay for development
- Test with CloudFlare relay for internet-scale validation
- Both use same moq-transport client code (standards-compliant)
- No code changes needed to switch between relays (just configuration)

---

## Implementation Phases

### Phase 0: Third-Party Integration & Build Plumbing (4-5 days)
**Goal:** Wire the vendored moq-ffi package into the Unreal build, mirror the LiveKit FFI workflow, and document refresh procedures.

**Tasks:**
- [ ] 0.1: Record the exact `lifelike-and-believable/moq-ffi` commit/hash used for the current drop inside `ThirdParty/moq-ffi/README.md` (or equivalent version file)
- [ ] 0.2: Verify headers/libs/DLLs exist for Win64 Release (`include`, `lib`, `bin`) and add placeholders for Linux/Mac builds
- [ ] 0.3: Update `Open3DTransportMoQ.Build.cs` to link against the vendored libraries (mirroring the LiveKit FFI pattern)
- [ ] 0.4: Extend `Shared/MoQFfiSupport` to load/validate `moq_ffi.dll`, emit actionable errors when missing, and surface version info in logs
- [ ] 0.5: Keep `O3D_WITH_TRANSPORT_MOQ` flag wiring but document it as controlling whether moq-ffi is linked/enabled
- [ ] 0.6: Produce a short “refresh guide” describing how to pull a new moq-ffi release (Rust toolchain required only for that process)
- [ ] 0.7: Run a clean Unreal build to confirm the transport module links against the vendored library even though networking remains stubbed

**Migration Notes for Phases 0-2:**
- **Reference only:** Earlier MoQProtocol/MoQTrackManager files remain as historical artifacts; the new implementation starts fresh
- **Goal of Phase 0:** Ensure moq-ffi is treated like other ThirdParty dependencies (LiveKit, WebRTC) so future work focuses on gameplay code, not FFI plumbing

### Phase 1: Unreal Wrappers on top of moq-ffi (6-8 days)
**Goal:** Build the lightweight Unreal-facing façade that owns moq-ffi handles, schedules callback processing, and exposes ergonomic helpers to the transport module.

**Tasks:**
- [ ] 1.1: Define RAII structs/classes (e.g., `FMoqSessionHandle`, `FMoqPublisherHandle`, `FMoqSubscriberHandle`) that manage the lifetimes of moq-ffi objects and enforce thread-safe access
- [ ] 1.2: Implement `FMoQSessionWrapper` that encapsulates connection parameters, startup/shutdown, and track registration using the moq-ffi C++ exports
- [ ] 1.3: Create an async dispatcher/worker that pumps moq-ffi callbacks off the Tokio runtime threads and routes them to Unreal (similar to the LiveKit event bridge)
- [ ] 1.4: Add structured error translation so moq-ffi status codes become `EOpen3DTransportMoQError` enums with contextual logging
- [ ] 1.5: Provide small unit/automation tests (or lightweight harness) that create/destroy sessions and simulate announce/subscribe flows without touching higher-level sender/receiver code

**Migration Notes:**
- No modifications to the legacy MoQProtocol/MoQTrackManager files—new wrappers live under `Source/Open3DTransportMoQ/Private/Shared`
- All Rust async concerns remain encapsulated inside moq-ffi; Unreal only needs to manage worker threads and callback queues

### Phase 2: Core Sender Implementation via moq-transport (6-8 days)
**Goal:** Working sender that publishes tracks and sends objects via moq-relay-ietf

**Tasks:**
- [ ] 2.1: Implement FO3DMoQSender skeleton with IOpen3DSender interface
- [ ] 2.2: Integrate `FMoQSessionWrapper` (moq-ffi layer) into sender
- [ ] 2.3: Implement Initialize() - parse relay URL and track configuration
- [ ] 2.4: Implement Start() - connect to moq-relay-ietf as publisher
  - Connect via WebTransport to relay URL
  - Perform MoQ handshake with publisher role
  - Announce mocap track (e.g., "mocap/session1/character1")
- [ ] 2.5: Implement Send() method - publish object to mocap track
  - Serialize SubjectList to FlatBuffers
  - Create MoQ object with sequence number and timestamp
  - Publish via moq-ffi wrappers
- [ ] 2.6: Implement async worker thread for FFI event polling
  - Poll moq-transport for async results
  - Dispatch callbacks to game thread
  - Handle connection state changes
- [ ] 2.7: Implement Tick() for connection health monitoring
  - Check moq-transport session state
  - Implement auto-reconnect with exponential backoff
- [ ] 2.8: Implement Stop() with graceful shutdown
  - Unannounce tracks
  - Close moq-transport session
  - Clean up FFI resources
- [ ] 2.9: Implement GetStats() using moq-transport metrics

**Migration Notes:**
- **No direct QUIC handling** - all QUIC/WebTransport managed by moq-transport
- **No custom stream multiplexing** - moq-transport handles streams
- **Relay-only mode** - no "server mode" for publishers
- All traffic routes through moq-relay-ietf

### Phase 3: Core Receiver Implementation via moq-transport (5-7 days)
**Goal:** Working receiver that subscribes to tracks and receives objects via moq-relay-ietf

**Tasks:**
- [ ] 3.1: Implement FO3DMoQReceiver skeleton with IOpen3DReceiver interface
- [ ] 3.2: Integrate `FMoQSessionWrapper` (moq-ffi layer) into receiver
- [ ] 3.3: Implement Initialize() - parse relay URL and track pattern configuration
- [ ] 3.4: Implement Start() - connect to moq-relay-ietf as subscriber
  - Connect via WebTransport to relay URL (same as publisher)
  - Perform MoQ handshake with subscriber role
  - Subscribe to track pattern (e.g., "mocap/session1/*")
- [ ] 3.5: Implement async worker thread for FFI event polling
  - Poll moq-transport for incoming objects
  - Queue objects for game thread consumption
  - Handle subscription state changes (OK/ERROR)
- [ ] 3.6: Implement Poll() method - dequeue objects from subscribed tracks
  - Retrieve next object from FFI queue
  - Deserialize FlatBuffers to SubjectList
  - Return to consumer (CharacterCaptureComponent)
- [ ] 3.7: Implement automatic resubscription on reconnect
  - Detect connection loss via moq-transport
  - Reconnect to relay with exponential backoff
  - Resubscribe to all tracks
- [ ] 3.8: Implement latency tracking per track
  - Extract timestamp from MoQ object
  - Calculate receive latency
  - Expose via GetStats()
- [ ] 3.9: Implement Stop() with graceful shutdown
  - Unsubscribe from all tracks
  - Close moq-transport session
  - Clean up FFI resources

**Migration Notes:**
- **No custom subscription logic** - moq-transport handles SUBSCRIBE/ANNOUNCE
- **No direct stream handling** - moq-transport manages object reception
- **Relay-only mode** - subscribers connect to relay, not directly to publishers

### Phase 4: Audio Support with MoQ Audio Tracks via moq-transport (4-6 days)
**Goal:** Full audio transmission capability using separate MoQ audio tracks

**Tasks:**
- [ ] 4.1: Implement FO3DMoQSenderAudioSink
- [ ] 4.2: Implement CreateAudioSink() factory in sender
- [ ] 4.3: Auto-announce audio track on CreateAudioSink() via FFI
  - Announce track with namespace "audio/session1/character1"
  - Configure track properties (priority=high, reliable)
- [ ] 4.4: Implement SubmitAudio() - publish audio objects to audio track
  - Encode audio (PCM16 or Opus) via O3DAudio framework
  - Create MoQ object with audio payload
  - Publish via moq-ffi wrappers
- [ ] 4.5: Implement receiver audio track subscription via FFI
  - Subscribe to audio track pattern (e.g., "audio/session1/*")
  - Register audio object callback
- [ ] 4.6: Implement receiver audio delivery via SetAudioSink()
  - Poll audio objects from FFI
  - Decode audio via O3DAudio framework
  - Deliver to registered audio sink
- [ ] 4.7: Audio track state management and error handling
  - Handle audio track disconnection/reconnection
  - Buffer audio to smooth jitter
  - Implement audio sync with mocap frames

**Migration Notes:**
- Audio tracks are separate MoQ tracks (not separate QUIC streams)
- moq-transport handles track multiplexing and prioritization
- Higher priority for audio tracks configured at track level

### Phase 5: moq-relay-ietf Deployment & Testing (3-4 days)
**Goal:** Deploy and test moq-relay-ietf for local and CloudFlare relay scenarios

**Tasks:**
- [ ] 5.1: Build moq-relay-ietf from moq-rs repository
  - Verify Rust build on Win64
  - Create deployment package (binary + dependencies)
- [ ] 5.2: Create local relay deployment guide
  - TLS certificate generation (self-signed for local)
  - Configuration file format
  - Command-line arguments
  - Listen address and port configuration
- [ ] 5.3: Deploy local relay for testing
  - Run moq-relay-ietf on localhost:4443
  - Verify WebTransport connectivity
  - Test with moq-pub/moq-sub reference clients
- [ ] 5.4: Test UE sender → local relay → UE receiver
  - Publisher connects to local relay
  - Subscriber connects to same relay
  - Verify track announcement and subscription
  - Verify object delivery
- [ ] 5.5: Document CloudFlare relay configuration
  - Obtain CloudFlare relay URL (experimental network)
  - Configure TLS trust anchors for CloudFlare certs
  - Document network requirements (ports, protocols)
- [ ] 5.6: Test UE clients with CloudFlare relay
  - Publisher connects to CloudFlare relay
  - Subscriber connects from different network
  - Verify internet-scale routing
  - Measure latency and throughput
- [ ] 5.7: Create relay monitoring and troubleshooting guide
  - Log analysis
  - Connection state debugging
  - Performance tuning

**Migration Notes:**
- **No custom relay implementation** - use moq-relay-ietf as-is
- Relay is external to UE plugin (separate process/server)
- Same client code works with local or CloudFlare relay (just URL changes)
- Phase 5 now deployment/testing focused, not development

### Phase 6: Configuration & Options for moq-relay-ietf (3-4 days)
**Goal:** Flexible configuration parsing with relay URL and MoQ track naming support

**Tasks:**
- [ ] 6.1: Define configuration keys in MoQHelpers.h
  - relay_url (e.g., "https://localhost:4443" or CloudFlare URL)
  - track_namespace (e.g., "mocap/session1/character1")
  - track_pattern (e.g., "mocap/session1/*" for subscribers)
  - track_priority (0-255, default 128)
  - track_reliability (reliable/unreliable)
- [ ] 6.2: Implement ParseSenderOptions()
  - Parse relay_url
  - Parse track_namespace for mocap track
  - Validate track naming (UE string to moq-transport format)
- [ ] 6.3: Implement ParseReceiverOptions()
  - Parse relay_url
  - Parse track_pattern for subscription
  - Support wildcard patterns (*, ?)
- [ ] 6.4: Implement configuration validation
  - Validate relay URL format (https://...)
  - Validate track names (namespace rules)
  - Provide helpful error messages
- [ ] 6.5: Add default configuration values
  - Default relay: https://localhost:4443
  - Default track namespace: mocap/default/character
  - Default pattern: mocap/default/*

**Migration Notes:**
- **No "client-server mode"** - always use relay_url
- Configuration simplified (no bind address for publishers)
- Track naming follows MoQ conventions (namespace/track)

### Phase 7: Editor UI Customization for Relay Configuration (3-4 days)
**Goal:** User-friendly Slate widgets with relay URL and track configuration

**Tasks:**
- [ ] 7.1: Implement SMoQSenderSettingsPanel
  - Relay URL input field (editable text box)
  - Relay URL presets dropdown (Localhost, CloudFlare)
  - Track namespace input (e.g., "mocap/session1/alice")
  - Track priority slider (0-255)
  - Reliability mode dropdown (Reliable/Unreliable)
  - Visual feedback for connection status
- [ ] 7.2: Implement SMoQReceiverSettingsPanel
  - Relay URL input field (same as sender)
  - Relay URL presets dropdown (Localhost, CloudFlare)
  - Track pattern input (e.g., "mocap/session1/*")
  - Pattern helper (shows matching examples)
  - Visual feedback for subscription status
  - List of active subscriptions
- [ ] 7.3: Add helpful UI elements
  - Relay selection guide (Local for dev, CloudFlare for testing)
  - Track naming best practices tooltip
  - Connection status indicator (connected, connecting, error)
  - Stats display (latency, throughput, objects received)
- [ ] 7.4: Register UI customizations in module startup
- [ ] 7.5: Test UI in Unreal Editor
  - Test with local relay
  - Test with CloudFlare relay (if available)
  - Verify all configuration options work

**Migration Notes:**
- **No mode selector** - relay-only architecture
- UI focused on relay URL and track configuration
- Provide good defaults for quick setup

### Phase 8: Module Registration & Integration (2-3 days)
**Goal:** Plugin discovers and uses MoQ transport automatically

**Tasks:**
- [ ] 8.1: Implement Open3DTransportMoQModule.cpp
- [ ] 8.2: Register transport factories
- [ ] 8.3: Register customizations
- [ ] 8.4: Update Open3DBroadcast.uplugin
- [ ] 8.5: Verify plugin loads

### Phase 9: Automated Testing with moq-relay-ietf Scenarios (6-8 days)
**Goal:** Comprehensive test coverage for FFI layer and relay-based communication

**Tasks:**
- [ ] 9.1: Update MoQTransportTests.cpp for FFI/relay architecture
- [ ] 9.2: Implement FFI layer tests (4 tests)
  - Session creation/destruction
  - Track announcement via FFI
  - Track subscription via FFI
  - Object publish/receive via FFI
- [ ] 9.3: Implement initialization tests (3 tests)
  - Valid relay URL succeeds
  - Invalid relay URL fails gracefully
  - Missing configuration uses defaults
- [ ] 9.4: Implement relay connection tests (4 tests)
  - Publisher connects to local relay
  - Subscriber connects to local relay
  - Connection loss triggers reconnect
  - Multiple concurrent connections
- [ ] 9.5: Implement MoQ track announcement tests (3 tests)
  - Announce mocap track
  - Announce audio track
  - Announce multiple tracks
- [ ] 9.6: Implement MoQ track subscription tests (4 tests)
  - Subscribe to specific track
  - Subscribe with wildcard pattern
  - Subscription receives announced tracks
  - Unsubscribe from track
- [ ] 9.7: Implement data transfer tests via relay (4 tests)
  - Single frame round-trip through relay
  - Multiple frames in order
  - Large payload handling
  - Frame loss detection
- [ ] 9.8: Implement multi-subscriber tests (3 tests)
  - 1 publisher → 2 subscribers via relay
  - 1 publisher → 5 subscribers via relay
  - 2 publishers → 3 subscribers (different tracks)
- [ ] 9.9: Implement audio track tests (3 tests)
  - Audio track announcement and subscription
  - Audio object delivery
  - Audio/mocap synchronization
- [ ] 9.10: Implement state management tests (2 tests)
  - GetStats returns accurate counts
  - Stop is idempotent
- [ ] 9.11: Implement error/edge case tests (3 tests)
  - Relay unavailable handling
  - Track not found error
  - FFI error propagation
- [ ] 9.12: Platform-specific tests

**Migration Notes:**
- **All tests require local moq-relay-ietf running**
- Tests start/stop local relay automatically (if possible)
- No "client-server" tests - all via relay
- Add FFI-specific tests for boundary crossing

### Phase 10: Documentation for moq-rs Integration (5-6 days)
**Goal:** Production-quality documentation covering moq-rs integration and relay deployment

**Tasks:**
- [ ] 10.1: Create README.md
  - Overview of moq-rs integration
  - IETF MoQ standards compliance
  - Relay-based architecture
  - Quick start with local relay
  - Quick start with CloudFlare relay
- [ ] 10.2: Create USER_GUIDE.md
  - Relay URL configuration (local vs CloudFlare)
  - Track naming conventions and best practices
  - Publisher setup (sender configuration)
  - Subscriber setup (receiver configuration with patterns)
  - Troubleshooting common issues
  - FAQ (15+ questions)
- [ ] 10.3: Create RELAY_DEPLOYMENT.md
  - Building moq-relay-ietf from moq-rs
  - Local relay deployment
    * TLS certificate setup
    * Configuration file
    * Running as service/daemon
  - CloudFlare relay setup
    * Obtaining access to experimental network
    * URL and endpoint configuration
    * Trust anchor configuration
  - Performance tuning
  - Monitoring and logging
  - Troubleshooting relay issues
- [ ] 10.4: Create FFI_ARCHITECTURE.md
  - Rust ↔ C ↔ C++ FFI design
  - Memory management across language boundary
  - Async/await bridging to Unreal threading
  - Error handling and propagation
  - Linking the vendored moq-ffi package (load paths, versioning, diagnostics)
- [ ] 10.5: Create IMPLEMENTATION_SUMMARY.md
  - moq-rs integration rationale
  - Architecture decisions
  - moq-transport API usage
  - FFI layer design
  - Test coverage summary
  - Performance analysis
  - Known limitations
  - Future enhancements
- [ ] 10.6: Create MOQ_STANDARDS_COMPLIANCE.md
  - IETF draft-ietf-moq-transport-07 compliance
  - Protocol message mapping
  - Track and object model
  - Interoperability notes
- [ ] 10.7: Update inline code documentation
  - Document FFI boundary functions
  - Document wrapper classes
  - Add usage examples
- [ ] 10.8: Create BUILD_GUIDE.md
  - Optional Rust toolchain setup (only for regenerating moq-ffi or moq-relay-ietf)
  - Steps to refresh the moq-ffi drop from https://github.com/lifelike-and-believable/moq-ffi
  - Rebuilding moq-relay-ietf for local testing
  - UE plugin build integration (linking vendored binaries, verifying DLL load)

**Migration Notes:**
- Documentation emphasizes standards compliance
- No "client-server vs relay" comparison (relay-only)
- Focus on local vs CloudFlare relay deployment
- Comprehensive FFI documentation for maintenance

### Phase 11: Build & Integration Testing (4-5 days)
**Goal:** Verify FFI integration, relay deployments, and real-world scenarios

**Tasks:**
- [ ] 11.1: Clean build verification
  - Verify vendored moq-ffi binaries are staged and loadable
  - Build moq-relay-ietf (only if artifacts changed)
  - Build UE plugin with moq-ffi linked in
  - Verify no build warnings
- [ ] 11.2: Local relay testing
  - Deploy moq-relay-ietf on localhost
  - Test 1 publisher → 1 subscriber
  - Test 1 publisher → 5 subscribers
  - Test 2 publishers → 3 subscribers (different tracks)
- [ ] 11.3: CloudFlare relay testing (if available)
  - Connect publisher to CloudFlare relay
  - Connect subscriber from different network
  - Verify internet-scale routing
  - Test cross-region latency
- [ ] 11.4: N:M production scenario testing
  - 2 publishers, 5 subscribers via local relay
  - Verify track isolation (subscribers only get their tracks)
  - Verify relay fan-out performance
- [ ] 11.5: Cross-transport interoperability testing
  - Test with NNG, WebRTC, Sockets transports
  - Verify Open3DBroadcast works with MoQ transport
  - Verify transport switching works
- [ ] 11.6: Audio end-to-end testing
  - Audio track announcement and subscription
  - Audio/mocap synchronization
  - Audio quality verification
- [ ] 11.7: Performance benchmarking
  - Local relay latency (should be <10ms)
  - CloudFlare relay latency (varies by region)
  - Throughput testing (30 FPS mocap + audio)
  - CPU usage profiling
  - Memory usage profiling
- [ ] 11.8: Relay stress testing
  - 10+ concurrent publishers
  - 20+ concurrent subscribers
  - Multi-track fanout
  - Connection churn (rapid connect/disconnect)
- [ ] 11.9: Interoperability with moq-pub/moq-sub
  - Verify UE publisher works with moq-sub
  - Verify moq-pub works with UE receiver
  - Validate standards compliance

**Migration Notes:**
- Testing focuses on relay scenarios (no "client-server")
- Emphasize FFI stability and error handling
- Validate standards compliance with reference clients

### Phase 12: Finalization & PR (2-3 days)
**Goal:** Code review, polish, and merge readiness

**Tasks:**
- [ ] 12.1: Code review preparation
  - Review FFI layer code quality
  - Review C++ wrapper code
  - Verify error handling completeness
  - Check for memory leaks (FFI boundary)
- [ ] 12.2: Documentation review
  - Review all documentation for accuracy
  - Verify moq-rs integration is well-explained
  - Check relay deployment guides
  - Ensure FFI architecture is documented
- [ ] 12.3: Testing final pass
  - Run all automation tests
  - Verify local relay scenarios
  - Test CloudFlare relay (if available)
  - Performance benchmarks meet targets
- [ ] 12.4: Security review
  - Review FFI layer for safety issues
  - Verify TLS certificate handling
  - Check for potential vulnerabilities
- [ ] 12.5: Changelog and version update
  - Document moq-rs integration
  - List breaking changes from custom protocol
  - Update O3DS_VERSION_TAG
- [ ] 12.6: Pull request creation
  - Comprehensive PR description
  - Link to documentation
  - Include performance benchmarks
  - Note moq-rs standards compliance
  - Document CloudFlare relay testing results

**Migration Notes:**
- PR should highlight moq-rs integration benefits
- Emphasize standards compliance and interoperability
- Document FFI layer for future maintenance

---

## Migration Strategy: From Custom Protocol to moq-ffi-backed Transport

### Current State (November 2025)

- A partial custom MoQ prototype (`MoQProtocol.*`, `MoQTrackManager.*`, `MoQTests.cpp`) exists but is no longer on the critical path
- `lifelike-and-believable/moq-ffi` delivers prebuilt bindings (Rust↔C/C++) and has already been vendored into `ThirdParty/moq-ffi`
- No production features rely on the prototype, making a restart low-risk

### Strategy Overview

1. **Archive, Don’t Refactor:** Keep the prototype sources for reference but disable them in the build. All new work targets brand-new files.
2. **Consume moq-ffi:** Treat moq-ffi like LiveKit’s FFI package—versioned binaries plus headers that are linked directly from the Unreal module.
3. **Greenfield Implementation:** Rebuild sender, receiver, audio, UI, and tests on top of the new `FMoQSessionWrapper` layer without carrying over legacy design constraints.

### Legacy Files to Sunset

- `MoQProtocol.h/cpp`
- `MoQTrackManager.h/cpp`
- `MoQTests.cpp`
- Any experimental QUIC/network helpers outside the new architecture

These files remain in source control for archeology but will not be modified unless specific helpers prove reusable (e.g., enums or config structs).

### New Assets

- `Shared/MoQFfiSupport.*` plus additional wrappers (`FMoQSessionWrapper`, publisher/subscriber helpers)
- `ThirdParty/moq-ffi` (headers, libs, DLLs, README with upstream commit)
- Updated module documentation outlining how to refresh third-party drops

### Step-by-Step Plan

**Step 1: Archive + Document**
- Move legacy sources out of the build target (or wrap them in `#if 0`)
- Document prior learnings so they can inform the restart

**Step 2: Validate Third-Party Assets**
- Confirm vendored binaries load correctly on Win64
- Create version/README metadata describing how to refresh from GitHub

**Step 3: Implement Wrappers + Core Transports**
- Build the Unreal-facing wrapper layer (Phase 1)
- Implement sender/receiver/audio flows (Phases 2-4)

**Step 4: Expand Testing + Tooling**
- Recreate/extend previous tests using the relay-only workflow
- Add automation that validates DLL presence and compatibility

### Breaking Changes from Original Plan

- **Architecture:** From custom protocol + msquic to moq-ffi (moq-transport via Rust) + relay-only networking
- **Code ownership:** From living inside this repo to depending on the external `moq-ffi` package for all cross-language concerns
- **Workflow:** From “build everything locally with Rust” to “link prebuilt binaries; only rebuild when refreshing ThirdParty”

### Benefits of the Restart

1. **Deterministic Builds:** UE projects simply link against known-good binaries
2. **Faster Timeline:** Eliminates weeks of FFI crate work already completed in the dedicated repo
3. **Cleaner Architecture:** Avoids retrofitting legacy classes—everything is designed around relay-only semantics from day one
4. **Shared Practices:** Mirrors the proven LiveKit FFI approach, simplifying developer onboarding

### Risks & Mitigation

- **Binary Drift:** Track upstream commit/hash inside `ThirdParty/moq-ffi/README.md` and add CI checks that warn when DLLs are missing or outdated.
- **Threading/Callback Surprises:** Clearly document the worker model in FFI_ARCHITECTURE.md and add automation to cover shutdown edge cases.
- **Platform Coverage:** Prepare Linux/Mac placeholder directories now so future ports only need fresh binaries, not code changes.

---

## File Structure

Complete file tree for Open3DTransportMoQ module with moq-rs integration:

```
ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportMoQ/
├── Open3DTransportMoQ.Build.cs          [Build configuration, links moq-ffi]
├── README.md                            [Module overview, moq-ffi integration]
├── USER_GUIDE.md                        [Configuration guide, relay setup]
├── RELAY_DEPLOYMENT.md                  [moq-relay-ietf deployment guide]
├── FFI_ARCHITECTURE.md                  [moq-ffi usage, threading, diagnostics]
├── IMPLEMENTATION_SUMMARY.md            [Architecture and design decisions]
├── MOQ_STANDARDS_COMPLIANCE.md          [IETF MoQ compliance documentation]
├── BUILD_GUIDE.md                       [How to refresh moq-ffi + relay builds]
│
├── Private/
│   ├── Open3DTransportMoQModule.cpp    [Module registration, transport factory]
│   │
│   ├── Shared/
│   │   ├── MoQFfiSupport.h/.cpp         [DLL loading, version checks, helpers]
│   │   ├── MoQSessionWrapper.h/.cpp     [RAII wrapper over moq-ffi session]
│   │   ├── MoQPublisher.h/.cpp          [Track announcement + publishing]
│   │   ├── MoQSubscriber.h/.cpp         [Subscription + receive queue]
│   │   ├── MoQAsyncDispatcher.h/.cpp    [Background worker pumping callbacks]
│   │   ├── MoQHelpers.h/.cpp            [Configuration parsing, logging]
│   │   └── MoQTypes.h                   [UE-friendly structs/enums]
│   │
│   ├── Sender/
│   │   ├── MoQSender.h/.cpp             [FO3DMoQSender implementation]
│   │   ├── MoQSenderAudioSink.h/.cpp    [Audio sink publishing support]
│   │   └── ...
│   │
│   ├── Receiver/
│   │   ├── MoQReceiver.h/.cpp           [FO3DMoQReceiver implementation]
│   │   └── ...
│   │
│   └── Tests/
│       ├── MoQTransportTests.cpp        [Relay e2e tests]
│       └── MoQFfiSmokeTests.cpp         [DLL + wrapper validation]
│
├── ThirdParty/
│   └── moq-ffi/
│       ├── README.md                    [Upstream commit/hash, refresh steps]
│       ├── include/moq_ffi.h            [Exported C header from moq-ffi repo]
│       ├── lib/Win64/Release/moq_ffi.lib
│       └── bin/Win64/Release/
│           ├── moq_ffi.dll
│           └── moq_ffi.pdb
│
└── Binaries/
  └── Win64/
    └── moq_ffi.dll                  [Copied/staged runtime binary]
```

**External Dependencies (Not in Plugin):**
```
moq-relay-ietf/                            [Deployed separately]
├── [Built from moq-rs repository]
└── [Runs as standalone server process]
```

**Note on Architecture:**
- **Relay-Only**: All traffic goes through moq-relay-ietf (local or CloudFlare)
- **FFI Layer**: Rust moq-transport ↔ C bindings ↔ C++ wrappers ↔ UE plugin
- **No custom relay**: Use moq-relay-ietf as-is from moq-rs

---

## Testing Strategy

### Test Coverage Requirements

**Total: 25+ automation tests**

**Category 1: FFI Layer Tests (4 tests)**
- Session creation and destruction
- Track announcement via FFI
- Track subscription via FFI
- Object publish/receive via FFI

**Category 2: Initialization (3 tests)**
- Valid relay URL succeeds
- Invalid relay URL fails gracefully
- Missing configuration uses defaults

**Category 3: Relay Connection (4 tests)**
- Publisher connects to local relay
- Subscriber connects to local relay
- Connection lost triggers reconnect
- Multiple concurrent connections

**Category 4: MoQ Track Operations (4 tests)**
- Announce mocap track
- Announce audio track
- Subscribe to specific track
- Subscribe with wildcard pattern

**Category 5: Data Transfer via Relay (4 tests)**
- Single frame round-trip through relay
- Multiple frames in order
- Large payload handling
- Multi-subscriber fanout (1:N)

**Category 6: Audio (3 tests)**
- SupportsAudio returns true
- CreateAudioSink returns valid sink
- Audio transmission end-to-end via relay

**Category 7: State & Stats (2 tests)**
- GetStats returns accurate counts
- Stop is idempotent

**Category 8: Error Handling (3 tests)**
- Relay unavailable handling
- Track not found error
- FFI error propagation

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
| FFI layer complexity | High | High | Start simple, iterate; create comprehensive FFI tests; document memory management |
| Rust toolchain setup | Medium | Medium | Provide clear build docs; consider pre-built binaries; add to CI/CD |
| Async/await bridging | Medium | High | Use proven async bridge patterns; thorough callback testing; dedicated tokio thread |
| moq-rs API changes | Low | Medium | Pin to specific git tag (draft-ietf-moq-transport-07); vendor if needed |
| TLS certificate complexity | High | Medium | Use test certs for local relay; document generation; CloudFlare handles its certs |
| moq-relay-ietf deployment | Medium | Medium | Docker containers; clear deployment guide; fallback to local testing |
| Audio sync issues | Low | High | Use separate MoQ track, reference WebRTC audio code |
| UE API changes in 5.7 | Low | High | Verify all APIs against UE 5.7 docs before use |
| FFI memory leaks | Medium | High | Valgrind/sanitizer testing; careful ownership documentation; extensive testing |
| CloudFlare relay access | Medium | Low | Start with local relay; CloudFlare is optional for internet-scale testing |

### Contingency Plans

**If FFI layer proves too complex:**
- Fallback 1: Use moq-rs as subprocess with IPC (less efficient but simpler)
- Fallback 2: Implement minimal MoQ subset in C++ (loses standards compliance)
- Fallback 3: Wait for official C/C++ MoQ library (future work)

**If Rust build fails:**
- Option 1: Provide pre-built FFI binaries for Win64
- Option 2: Cross-compile on CI/CD, ship binaries
- Option 3: Docker-based build environment

**If moq-relay-ietf is problematic:**
- Option 1: Use local relay only, defer CloudFlare testing
- Option 2: Implement minimal relay (defeats purpose of moq-rs integration)
- Option 3: Test with other public MoQ relays if available

**If audio sync fails:**
- Ship without audio initially, add in follow-up PR
- Audio is optional (SupportsAudio() = false is valid)

**If standards compliance is blocking:**
- Ship with best-effort MoQ compatibility
- Document deviations from IETF draft
- Plan future update for full compliance

---

## Success Criteria

### Minimum Viable Product (MVP)

- [ ] Module compiles cleanly with O3D_WITH_TRANSPORT_MOQ=1
- [ ] Vendored moq-ffi binaries load and link correctly
- [ ] moq-relay-ietf deploys locally
- [ ] Sender and receiver implement all interface methods
- [ ] Publisher connects to local relay successfully
- [ ] Subscriber connects to local relay successfully
- [ ] Mocap data transmits reliably at 30 FPS for 5+ minutes via relay
- [ ] Audio transmits (PCM16 minimum) without stuttering via relay
- [ ] UI appears in editor for both sender and receiver
- [ ] At least 15 automation tests pass (including FFI tests)
- [ ] README, USER_GUIDE, FFI_ARCHITECTURE, RELAY_DEPLOYMENT exist and accurate

### Production Ready (Goal)

- [ ] All 25+ automation tests pass
- [ ] FFI layer has no memory leaks (verified with sanitizers)
- [ ] Performance meets targets (< 10ms latency via local relay, < 7% CPU)
- [ ] Zero warnings on clean build (C++ sources + moq-ffi integration)
- [ ] Standards compliance validated with moq-pub/moq-sub
- [ ] Documentation complete with troubleshooting and FAQ
- [ ] Runs stable for 60+ minutes in editor
- [ ] Code review feedback addressed
- [ ] N:M fanout works (1 publisher → 5+ subscribers)
- [ ] Benchmarks show QUIC via relay is competitive with NNG

### Stretch Goals (Nice-to-Have)

- [ ] CloudFlare relay integration working
- [ ] Linux support (requires refreshed moq-ffi binaries for Linux)
- [ ] Mac support (requires refreshed moq-ffi binaries for Mac)
- [ ] Pre-built FFI binaries for all platforms
- [ ] Interoperability with other MoQ clients demonstrated
- [ ] Live latency graph in editor
- [ ] Advanced relay stats monitoring in UI
- [ ] Certificate management UI helper

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

### moq-rs Resources

**Primary Repository:**
- GitHub: https://github.com/cloudflare/moq-rs
- Branch: draft-ietf-moq-transport-07
- License: MIT OR Apache-2.0

**Key Components:**
- moq-transport crate: https://crates.io/crates/moq-transport
- moq-transport docs: https://docs.rs/moq-transport/latest/moq_transport/
- moq-relay-ietf: Server in moq-rs repo
- moq-pub/moq-sub: Reference client examples

**IETF Standards:**
- MoQ Transport Draft: https://datatracker.ietf.org/doc/draft-ietf-moq-transport/
- MoQ Working Group: https://github.com/moq-wg/moq-transport
- QUIC Video Info: https://quic.video

**FFI Resources:**
- Rust FFI Guide: https://doc.rust-lang.org/nomicon/ffi.html
- cbindgen: https://github.com/mozilla/cbindgen
- cxx crate: https://cxx.rs/ (alternative to manual FFI)
- Tokio async runtime: https://tokio.rs/

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

**Total Duration:** 4-5 weeks (80-100 hours)

### Week 1: FFI Infrastructure
- Days 1-3: Setup & dependencies with moq-rs (Phase 0)
- Days 4-5: FFI layer foundation (Phase 1 start)

### Week 2: FFI Layer & Sender
- Days 1-2: Complete FFI layer (Phase 1 complete)
- Days 3-5: Core sender with moq-transport (Phase 2)

### Week 3: Receiver & Audio
- Days 1-3: Core receiver with moq-transport (Phase 3)
- Days 4-5: Audio support via MoQ tracks (Phase 4)

### Week 4: Relay & Testing
- Days 1-2: moq-relay-ietf deployment (Phase 5)
- Days 3-5: Configuration, UI, automated tests (Phases 6, 7, 9)

### Week 5: Documentation & QA
- Days 1-3: Comprehensive documentation (Phase 10)
- Days 4-5: Integration testing and PR (Phases 11, 12)

**Contingency:** +1 week buffer for FFI complexity and relay testing

**Additional Time for moq-rs Integration:**
- FFI layer development: +7-10 days
- Rust toolchain setup and learning curve: +2-3 days
- Standards compliance validation: +3-4 days
- Total additional time: ~2 weeks compared to custom implementation

---

**Plan Status:** READY FOR IMPLEMENTATION  
**Plan Version:** 1.0  
**Last Updated:** 2025-11-21  
**Created By:** Planning Agent  
**Review Status:** Ready for Coding Agent

---

**NOTE FOR CODING AGENT:**

This plan reflects the updated strategy to integrate Cloudflare's moq-rs implementation instead of building a custom MoQ protocol. The architecture is now:
- **Protocol**: IETF draft-ietf-moq-transport-07 via moq-rs
- **Language Bridge**: Rust (moq-transport) ↔ C (FFI) ↔ C++ (Unreal)
- **Relay**: moq-relay-ietf from moq-rs (no custom relay)
- **Deployment**: Relay-only (local or CloudFlare hosted)

**Before starting:**
1. Read this entire updated document
2. Study the existing NNG and WebRTC transport implementations
3. Verify UE 5.7 API compatibility for all planned APIs
4. Set up Rust toolchain (rustc 1.70+, cargo)
5. Clone and build moq-rs (branch: draft-ietf-moq-transport-07)
6. Review existing MoQProtocol.h/cpp and MoQTrackManager.h/cpp (Phase 0-2 work)
7. Understand FFI design patterns (Rust Nomicon, cbindgen docs)

**During implementation:**
1. Follow phases in order (0 → 12)
2. **Phase 0-1 are critical** - FFI layer is foundation for everything
3. Test FFI boundary extensively before building higher layers
4. Document memory management across language boundaries carefully
5. Test each phase before moving to the next
6. Deploy local moq-relay-ietf early for testing
7. Update this plan if you discover issues
8. Ask for clarification if blocked > 4 hours

**Key principles:**
- **FFI safety first** - memory leaks and crashes are critical
- Small steps > big leaps
- Test early, test often (especially FFI boundary)
- Never guess UE APIs or Rust FFI patterns - always verify
- Reference existing transports when uncertain
- Document tradeoffs and architectural decisions
- Pin moq-rs to specific commit for stability

**Critical Success Factors:**
- FFI layer must be rock-solid (memory management, error handling)
- Async bridging between Rust and UE must be reliable
- moq-relay-ietf must deploy easily for testing
- Standards compliance enables CloudFlare relay integration

**Migration Notes:**
- Existing MoQProtocol.h/cpp work is preserved as UE-friendly wrapper
- MoQTrackManager refactored to delegate to FFI layer
- No direct QUIC/stream handling - moq-transport handles it
- Tests updated for FFI and relay architecture

Good luck! 🚀 This is a more complex integration but yields standards compliance and CloudFlare relay access.
