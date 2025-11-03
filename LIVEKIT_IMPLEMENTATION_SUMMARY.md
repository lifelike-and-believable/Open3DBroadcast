# LiveKit Backend Implementation - Executive Summary

**Status:** Planning Complete - Ready for Implementation  
**Date:** 2025-11-03  
**Planning Agent:** Open3DStream Planning Agent  
**Estimated Duration:** 7-12 weeks (single engineer)

---

## Overview

This document summarizes the plan to implement LiveKit SFU backend support for Open3DStream, enabling scalable multi-party broadcast of real-time animation and audio data. The implementation follows the Open3DStream Agent Playbook principles to ensure minimal changes, backward compatibility, and maintainable code.

## Problem Statement

The current Open3DStream WebRTC implementation uses libdatachannel for peer-to-peer connectivity, which works well for 1-to-1 scenarios but doesn't scale efficiently to multi-party use cases (1-to-many, many-to-many). LiveKit's Selective Forwarding Unit (SFU) architecture provides:

- **Scalability:** Efficient media relay for 10+ simultaneous participants
- **Cloud-Native:** Production-ready server with managed hosting options
- **Simplified NAT Traversal:** SFU handles STUN/TURN complexity
- **JWT Authentication:** Secure, token-based access control
- **Rich Features:** Recording, egress, analytics, etc. (future phases)

## Implementation Approach

### Core Architecture

The implementation maintains the existing backend-agnostic design:

```
┌─────────────────────────────────────────────────────────┐
│  UO3DSBroadcastComponent / FOpen3DStreamSource          │
│  (No changes required)                                  │
└──────────────────────┬──────────────────────────────────┘
                       │
                       │ Uses
                       ▼
┌─────────────────────────────────────────────────────────┐
│  IWebRTCConnector Interface                             │
│  (Backend-agnostic contract)                            │
└──────────────────────┬──────────────────────────────────┘
                       │
                       │ Implemented by
           ┌───────────┴──────────┐
           ▼                      ▼
┌─────────────────────┐  ┌─────────────────────┐
│ FLibDataChannelConn │  │ FLiveKitConnector   │
│ (Existing P2P)      │  │ (New SFU)          │
└─────────────────────┘  └─────────────────────┘
```

**Key Principle:** The Broadcaster and Receiver components remain unchanged. They interact only through the IWebRTCConnector interface, with the factory pattern handling backend instantiation.

### SDK Selection

**Recommendation:** Use **zesun96/livekit-client-cpp** (community SDK)

**Rationale:**
- More mature than official SDK (currently marked WIP)
- Cross-platform support proven (Windows, Linux, macOS, iOS, Android)
- Working examples for audio/video publishing
- Better documentation
- Can migrate to official SDK when production-ready

**Alternative:** Monitor official SDK progress and migrate when stable

### Data Mapping

LiveKit uses topic-based data messages instead of arbitrary DataChannels:

| O3DS Data Type | LibDataChannel | LiveKit |
|----------------|----------------|---------|
| Animation frames | DataChannel (lossy) | `o3ds.anim` topic (lossy) |
| Control messages | DataChannel (reliable) | `o3ds.ctrl` topic (reliable) |
| Audio metadata | DataChannel (reliable) | `o3ds.audio.announce` topic (reliable) |

**Header Format:** All messages include `{topic, version, seq, timestamp, subject?, stream?}` for routing and drop logic.

### Audio Tracks

Both backends use the same subject labeling conventions:

- **Game Mix:** Track name = `o3ds:mix`
- **Per-Subject Mic:** Track name = `o3ds:subject/<SubjectName>`

This ensures consistent behavior from the Receiver's perspective, regardless of backend.

### Testing Strategy

**Ground Truth Test First:**

Following WEBRTC_TESTING_GUIDE.md, validate connectivity using `UO3DSWebRTCConnectorComponent` before full integration:

1. Deploy LiveKit server (Docker)
2. Generate test tokens
3. Create test level with two WebRTCConnectorComponents
4. Verify bidirectional data and audio flow
5. Confirm behavior matches LibDataChannel

**Then Integration Tests:**

Two-editor setup with full Broadcaster + Receiver to validate end-to-end animation and audio streaming.

## Implementation Phases

### Phase 1: SDK Integration (3-5 days)
- Evaluate and select SDK
- Add as thirdparty dependency (submodule or vendor)
- Update Open3DShared.Build.cs with conditional linking
- Add O3DS_ENABLE_LIVEKIT build flag
- Validate builds with flag on/off

### Phase 2: Core Implementation (10-15 days)
- Create FLiveKitConnector class
- Implement IWebRTCConnector interface methods:
  - Start/Stop/Tick lifecycle
  - Send() with topic routing
  - SendAudioPcm16() with Opus encoding
  - State/Data/RTP delegates
- Handle room connection and JWT authentication
- Implement remote participant tracking
- Add error handling and recovery

### Phase 3: Integration (2-3 days)
- Update FWebRTCConnectorFactory
- Wire config from Broadcast Component
- Wire config from LiveLink Source
- Validate backend selection UI
- Test config validation

### Phase 4: Testing Infrastructure (5-7 days)
- WebRTCConnectorComponent ground truth tests
- Unit tests for connector lifecycle
- Integration tests (two-editor setup)
- Multi-participant scenarios
- Performance benchmarking

### Phase 5: End-to-End Validation
- Validate animation streaming
- Validate audio streaming and playback
- Test reconnection and resilience
- Verify behavior parity with LibDataChannel
- Performance profiling

### Phase 6: Documentation (3-4 days)
- Update WEBRTC_TESTING_GUIDE.md
- Create backend comparison guide
- Document LiveKit deployment
- Add troubleshooting section
- Document token generation workflow

**Total Estimated Duration:** 7-12 weeks (including risk buffer)

## Key Deliverables

1. **FLiveKitConnector Implementation**
   - Full IWebRTCConnector interface support
   - JWT token authentication
   - Topic-based data messaging
   - Multi-track audio publishing/subscription
   - State management and error handling

2. **Build System Updates**
   - O3DS_ENABLE_LIVEKIT conditional compilation
   - SDK linking for Win64, Linux, macOS
   - Optional feature (builds without SDK installed)

3. **Testing Suite**
   - WebRTCConnectorComponent validation tests
   - Unit tests for connector methods
   - Integration tests for E2E scenarios
   - Performance benchmarks

4. **Documentation**
   - Implementation plan (36KB detailed spec)
   - Updated testing guide with LiveKit procedures
   - Backend comparison and selection guide
   - Deployment and token generation docs
   - Troubleshooting guide

## Success Criteria

### Functional
- ✓ LiveKit connector fully implements IWebRTCConnector
- ✓ Broadcaster publishes animation + audio via LiveKit
- ✓ Receiver subscribes to animation + audio via LiveKit
- ✓ Multi-participant support (1-to-many, many-to-many)
- ✓ Behavior parity with LibDataChannel (transparent to users)
- ✓ Clean connection/disconnection with proper error handling
- ✓ Automatic reconnection on network interruptions

### Performance
- ✓ Connection time < 5 seconds
- ✓ End-to-end latency < 200ms (acceptable for SFU)
- ✓ Data throughput 10-50 KB/s
- ✓ Audio packet rate ~50 pkt/s
- ✓ CPU usage < 5% sender, < 3% receiver
- ✓ Scales to 10+ receivers without degradation

### Quality
- ✓ Follows Open3DStream Agent Playbook
- ✓ Zero impact on existing LibDataChannel code
- ✓ Module isolation (all code in Open3DShared)
- ✓ Comprehensive testing (unit + integration)
- ✓ Complete documentation
- ✓ Cross-platform builds (Windows, Linux, Mac)
- ✓ Security best practices (token handling)

## Risk Mitigation

### Technical Risks

**SDK Maturity**
- Community SDK may have bugs
- **Mitigation:** Thorough evaluation in Phase 1, keep abstraction thin for easy replacement

**Build Complexity**
- Many dependencies (libwebrtc, protobuf, etc.)
- **Mitigation:** Consider vendoring pre-built binaries, document exact versions

**API Differences**
- Data messages vs DataChannels have different semantics
- **Mitigation:** Queue-based backpressure simulation, thorough load testing

**Performance**
- SFU adds latency vs P2P
- **Mitigation:** Early benchmarking, document expected increase, optimize relay paths

### Operational Risks

**Token Management**
- JWT tokens require secure generation
- **Mitigation:** Never embed secrets in builds, provide token service reference implementation

**Server Deployment**
- Teams may struggle with setup
- **Mitigation:** Docker compose for dev, AWS guide for production, recommend LiveKit Cloud

### Project Risks

**Scope Creep**
- Feature requests beyond core connectivity
- **Mitigation:** Clearly defined non-goals, stick to interface contract, defer to Phase 2

**Backward Compatibility**
- Changes break existing users
- **Mitigation:** Zero changes to LibDataChannel paths, extensive regression testing

## Dependencies

### External
- LiveKit C++ SDK (zesun96/livekit-client-cpp)
- libwebrtc (dependency of SDK)
- protobuf (for signaling)
- libwebsockets (for WebSocket)
- Opus codec (already used in LibDataChannel)

### Internal
- Open3DShared module (target for implementation)
- IWebRTCConnector interface (already defined)
- WebRTCConnectorFactory (small update)
- UO3DSWebRTCConnectorComponent (for testing)

## Related Documentation

- **Detailed Plan:** `plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md`
- **Testing Guide:** `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md`
- **Backend Comparison:** `plugins/unreal/Open3DStream/docs/M3.4.1a_Docs/WEBRTC_BACKENDS.md`
- **Data Messaging:** `plugins/unreal/Open3DStream/docs/M3.4.1a_Docs/LIVEKIT_DATA_MESSAGING.md`
- **LiveKit Deployment:** `LiveKit/README.md`

## Related Issues

- **#90:** M3.4.2 Phase 1: LiveKit Setup and Unreal Open3DStream Integration
- **#94:** M3.4.1a Refactor WebRTC transport: backend-agnostic interface, LiveKit SFU support
- **#88:** M3.4.3 Phase 2: VR/Interactive Broadcast at Scale (future)
- **#87:** WebRTC late-join/reconnect: make P2P resilient and plan LiveKit transport (closed)

## Team Guidance

### For Implementers

1. **Start with SDK evaluation** - Build and test both candidate SDKs before committing
2. **Follow the detailed plan** - Detailed implementation plan has complete class structure and method signatures
3. **Test incrementally** - Use WebRTCConnectorComponent for rapid iteration
4. **Keep abstraction thin** - SDK may need replacement, don't over-engineer
5. **Document as you go** - Update guides with actual findings

### For Reviewers

1. **Check module isolation** - All code must stay in Open3DShared
2. **Verify backward compatibility** - LibDataChannel code unchanged
3. **Test both backends** - Ensure factory correctly instantiates each
4. **Validate security** - No API secrets in code, token handling proper
5. **Review documentation** - Must enable team to deploy and use

### For QA

1. **Test ground truth first** - WebRTCConnectorComponent validates connectivity
2. **Then integration** - Full broadcaster + receiver E2E
3. **Multi-participant** - Scale to 10+ receivers
4. **Stress test** - Network interruptions, late-join, reconnection
5. **Performance** - Compare benchmarks to LibDataChannel

## Next Steps

1. **Approve Plan** ✓ (this document)
2. **SDK Evaluation** (Phase 1 start)
   - Clone zesun96/livekit-client-cpp
   - Build on Windows, Linux, macOS
   - Test basic room connection
   - Document findings
3. **Kickoff Implementation** (Phase 2)
   - Create FLiveKitConnector skeleton
   - Implement Start/Stop methods
   - Test connection and auth
4. **Iterative Development** (Phases 3-6)
   - Feature by feature
   - Test after each addition
   - Update docs continuously
5. **Final Validation**
   - Full regression test
   - Performance benchmarks
   - Documentation review
   - Team acceptance

---

## Conclusion

This implementation plan provides a clear, structured path to adding LiveKit SFU backend support to Open3DStream while maintaining the project's high standards for code quality, backward compatibility, and documentation. The phased approach allows for incremental progress with validation at each step, reducing risk and ensuring a successful outcome.

The estimated 7-12 week timeline is realistic given the scope and includes appropriate risk buffers. The plan follows all Open3DStream Agent Playbook principles and provides sufficient detail for an experienced Unreal/C++ engineer to execute the implementation.

**Ready for implementation approval and resource allocation.**

---

**For Questions or Discussion:**
- Review detailed plan: `plugins/unreal/Open3DStream/docs/LIVEKIT_BACKEND_IMPLEMENTATION_PLAN.md`
- Consult existing guides: `plugins/unreal/Open3DStream/docs/`
- Reference issues: #90, #94

