# Open3DTransportQUIC - Complete Implementation Plan

**Issue Type:** Feature Implementation  
**Priority:** High  
**Estimated Complexity:** Large (3-4 weeks)  
**Dependencies:** msquic library (recommended), UE 5.6 build system  
**Target Platform:** Win64 (initially), architecturally ready for Linux/Mac

---

## Executive Summary

Implement a complete QUIC transport module (`Open3DTransportQUIC`) for the Open3DBroadcast plugin, following the established architectural patterns from existing transports (NNG, WebRTC, Sockets, Loopback). QUIC provides modern connection-oriented transport with multiplexing, low latency, and built-in encryption, filling a gap between basic TCP/UDP and heavyweight WebRTC solutions.

### Key Objectives

1. **Complete interface compliance** with `IOpen3DSender` and `IOpen3DReceiver`
2. **Full audio support** using O3DAudio framework (PCM16/Opus)
3. **Editor UI customization** for host/port/stream configuration
4. **Comprehensive testing** with UE automation tests
5. **Production-ready documentation** (README, USER_GUIDE, IMPLEMENTATION_SUMMARY)
6. **Platform support**: Win64 first, Linux/Mac architectural support

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
- Unreal Engine 5.6 (verified API compatibility)
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

1. **Modular isolation**: Self-contained module with no changes to core Open3D* modules
2. **Interface compliance**: 100% parity with IOpen3DSender/IOpen3DReceiver contracts
3. **Threading model**: Async I/O with dedicated worker threads (following NNG pattern)
4. **Stream multiplexing**: Separate QUIC streams for mocap data and audio
5. **Backpressure handling**: Queue-based buffering with overflow detection
6. **Connection management**: Auto-reconnect with exponential backoff

### QUIC Stream Strategy

**Stream 0: Control Channel**
- Bidirectional, reliable
- Connection handshake and metadata exchange
- Protocol version negotiation

**Stream 1: Mocap Data (Primary)**
- Unidirectional, unreliable with QUIC datagrams (for low latency)
- Fallback to reliable ordered stream if datagram size exceeded
- Similar to WebRTC's unreliable datachannel with reliable fallback

**Stream 2: Audio Data**
- Unidirectional, reliable ordered
- PCM16 or Opus encoded frames
- Separate from mocap to prevent head-of-line blocking

### Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                    Open3DTransportQUIC Module                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌──────────────────┐              ┌──────────────────┐         │
│  │  FO3DQuicSender  │              │ FO3DQuicReceiver │         │
│  │                  │              │                  │         │
│  │ - Initialize()   │              │ - Initialize()   │         │
│  │ - Start()        │              │ - Start()        │         │
│  │ - Stop()         │              │ - Stop()         │         │
│  │ - Send()         │              │ - Poll()         │         │
│  │ - Tick()         │              │ - SetConsumer()  │         │
│  │ - CreateAudio    │              │ - SetAudioSink() │         │
│  │   Sink()         │              │                  │         │
│  └────────┬─────────┘              └────────┬─────────┘         │
│           │                                 │                    │
│           └────────┬────────────────────────┘                    │
│                    │                                             │
│         ┌──────────▼──────────┐                                 │
│         │  FQuicConnection    │                                 │
│         │  (Wrapper)          │                                 │
│         │                     │                                 │
│         │ - QUIC_HANDLE       │                                 │
│         │ - Stream Management │                                 │
│         │ - Event Callbacks   │                                 │
│         └──────────┬──────────┘                                 │
│                    │                                             │
│         ┌──────────▼──────────┐                                 │
│         │   msquic Library    │                                 │
│         │   (ThirdParty/)     │                                 │
│         └─────────────────────┘                                 │
│                                                                   │
│  ┌──────────────────────────────────────────────────────┐       │
│  │           Editor UI Customization                     │       │
│  │  - SQuicSenderSettingsPanel (Slate widget)           │       │
│  │  - SQuicReceiverSettingsPanel (Slate widget)         │       │
│  └──────────────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────────────┘
```

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

### Phase 1: Core Sender Implementation (5-7 days)
**Goal:** Working sender that can establish QUIC connections and send mocap data

**Tasks:**
- [ ] 1.1: Implement FQuicConnection wrapper class
- [ ] 1.2: Implement FO3DQuicSender skeleton
- [ ] 1.3: Implement connection establishment
- [ ] 1.4: Implement Send() method
- [ ] 1.5: Implement async send worker thread (following NNG pattern)
- [ ] 1.6: Implement Tick() and Stop()

### Phase 2: Core Receiver Implementation (4-6 days)
**Goal:** Working receiver that can accept connections and receive mocap data

**Tasks:**
- [ ] 2.1: Implement FO3DQuicReceiver skeleton
- [ ] 2.2: Implement connection acceptance (server mode)
- [ ] 2.3: Implement Poll() method
- [ ] 2.4: Implement reconnection logic
- [ ] 2.5: Implement latency tracking

### Phase 3: Audio Support (4-6 days)
**Goal:** Full audio transmission capability for both sender and receiver

**Tasks:**
- [ ] 3.1: Implement FO3DQuicSenderAudioSink
- [ ] 3.2: Implement CreateAudioSink() factory
- [ ] 3.3: Implement receiver audio delivery
- [ ] 3.4: Implement SetAudioSink() on receiver
- [ ] 3.5: Audio error handling

### Phase 4: Configuration & Options (3-4 days)
**Goal:** Flexible configuration parsing matching existing transports

**Tasks:**
- [ ] 4.1: Define configuration keys in QuicHelpers.h
- [ ] 4.2: Implement ParseSenderOptions() in QuicHelpers.cpp
- [ ] 4.3: Implement ParseReceiverOptions() in QuicHelpers.cpp
- [ ] 4.4: Implement MakeStreamId()

### Phase 5: Editor UI Customization (3-4 days)
**Goal:** User-friendly Slate widgets for sender and receiver configuration

**Tasks:**
- [ ] 5.1: Implement SQuicSenderSettingsPanel (Slate widget)
- [ ] 5.2: Implement SQuicReceiverSettingsPanel (Slate widget)
- [ ] 5.3: Register UI customizations in module startup
- [ ] 5.4: Test UI in Unreal Editor

### Phase 6: Module Registration & Integration (2-3 days)
**Goal:** Plugin discovers and uses QUIC transport automatically

**Tasks:**
- [ ] 6.1: Implement Open3DTransportQUICModule.cpp
- [ ] 6.2: Register transport factories
- [ ] 6.3: Register customizations
- [ ] 6.4: Update Open3DBroadcast.uplugin
- [ ] 6.5: Verify plugin loads

### Phase 7: Automated Testing (4-5 days)
**Goal:** Comprehensive test coverage following UE automation test pattern

**Tasks:**
- [ ] 7.1: Create QuicTransportTests.cpp
- [ ] 7.2: Implement initialization tests (3 tests)
- [ ] 7.3: Implement connection tests (4 tests)
- [ ] 7.4: Implement data transfer tests (3 tests)
- [ ] 7.5: Implement audio tests (3 tests)
- [ ] 7.6: Implement state management tests (2 tests)
- [ ] 7.7: Implement error/edge case tests (2 tests)
- [ ] 7.8: Platform-specific tests

### Phase 8: Documentation (3-4 days)
**Goal:** Production-quality user and developer documentation

**Tasks:**
- [ ] 8.1: Create README.md
- [ ] 8.2: Create USER_GUIDE.md
- [ ] 8.3: Create IMPLEMENTATION_SUMMARY.md
- [ ] 8.4: Inline code documentation

### Phase 9: Build & Integration Testing (2-3 days)
**Goal:** Verify module builds and runs in realistic scenarios

**Tasks:**
- [ ] 9.1: Clean build verification
- [ ] 9.2: Runtime testing
- [ ] 9.3: Cross-transport interoperability
- [ ] 9.4: Audio end-to-end test
- [ ] 9.5: Performance benchmarking

### Phase 10: Finalization & PR (2-3 days)
**Goal:** Code review, polish, and merge readiness

**Tasks:**
- [ ] 10.1: Code review preparation
- [ ] 10.2: Documentation review
- [ ] 10.3: Testing final pass
- [ ] 10.4: Changelog and version update
- [ ] 10.5: Pull request

---

## File Structure

Complete file tree for Open3DTransportQUIC module:

```
ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportQUIC/
├── Open3DTransportQUIC.Build.cs          [Build configuration, msquic linkage]
├── README.md                              [Module overview, quick start]
├── USER_GUIDE.md                          [End-user configuration guide]
├── IMPLEMENTATION_SUMMARY.md              [Architecture and decisions]
│
├── Private/
│   ├── Open3DTransportQUICModule.cpp      [Module registration, transport factory]
│   │
│   ├── Sender/
│   │   ├── QuicSender.h                   [FO3DQuicSender class declaration]
│   │   ├── QuicSender.cpp                 [IOpen3DSender implementation]
│   │   ├── QuicSenderAudioSink.h          [FO3DQuicSenderAudioSink class]
│   │   └── QuicSenderAudioSink.cpp        [Audio encoding and transmission]
│   │
│   ├── Receiver/
│   │   ├── QuicReceiver.h                 [FO3DQuicReceiver class declaration]
│   │   └── QuicReceiver.cpp               [IOpen3DReceiver implementation]
│   │
│   ├── Shared/
│   │   ├── QuicHelpers.h                  [Configuration parsing, utilities]
│   │   ├── QuicHelpers.cpp                [ParseSenderOptions, ParseReceiverOptions]
│   │   ├── QuicConnection.h               [RAII wrapper for QUIC_HANDLE]
│   │   └── QuicConnection.cpp             [Connection lifecycle management]
│   │
│   └── Tests/
│       └── QuicTransportTests.cpp         [17+ automation tests]
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
| UE API changes in 5.6 | Low | High | Verify all APIs against UE 5.6 docs before use |
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

### UE 5.6 API References

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
3. Verify UE 5.6 API compatibility for all planned APIs
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
