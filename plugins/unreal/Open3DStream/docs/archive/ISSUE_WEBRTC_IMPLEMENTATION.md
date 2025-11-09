# Issue: Implement WebRTC Functionality in Unreal Plugin Using libdatachannel

## Summary

Implement functional WebRTC support in the Open3DStream Unreal plugin using the libdatachannel libraries integrated in Issue #13. Currently, the plugin has the build infrastructure (libraries, headers, linking) but no actual WebRTC implementation - the connector is a stub that returns errors.

## Current State (After Issue #13)

### ✅ What We Have
- libdatachannel 0.23.2 static libraries built with MbedTLS 3.6.5
- Libraries linked in all platforms: `datachannel.lib` (Windows), `libdatachannel.a` (Linux/macOS)
- `RTC_STATIC=1` defined in Build.cs
- Headers available in `plugins/unreal/Open3DStream/ThirdParty/webrtc/include/rtc/`
- CI workflow for rebuilding libraries when needed
- UI shows "WebRTC Client" and "WebRTC Server" protocol options

### ❌ What's Missing
- No libdatachannel API usage in any code
- WebRTC connector is a stub with TODO comments
- No peer connection creation
- No data channel implementation
- No signaling protocol
- Always returns error: "WebRTC support via Pixel Streaming is not yet implemented"

## Requirements

### Functional Requirements

#### FR1: WebRTC Client Mode
**As an** Unreal user  
**I want to** select "WebRTC Client" protocol in LiveLink  
**So that** I can receive animation data from a remote WebRTC server with NAT traversal

**Acceptance Criteria:**
- [ ] User selects "WebRTC Client" from protocol dropdown
- [ ] User enters URL: `webrtc://signaling-server.com:8080/room-name`
- [ ] Plugin connects to signaling server via WebSocket
- [ ] Plugin creates `rtc::PeerConnection` as client
- [ ] Plugin negotiates connection with remote peer
- [ ] Plugin receives animation data over WebRTC data channel
- [ ] Animation data is passed to LiveLink for playback
- [ ] Connection status is displayed in LiveLink UI

#### FR2: WebRTC Server Mode
**As an** Unreal user  
**I want to** select "WebRTC Server" protocol in LiveLink  
**So that** I can accept incoming WebRTC connections from animation sources

**Acceptance Criteria:**
- [ ] User selects "WebRTC Server" from protocol dropdown
- [ ] User enters URL: `webrtc://0.0.0.0:8080/room-name`
- [ ] Plugin connects to signaling server as server
- [ ] Plugin creates `rtc::PeerConnection` as server
- [ ] Plugin accepts connection from remote peer
- [ ] Plugin receives animation data over WebRTC data channel
- [ ] Animation data is passed to LiveLink for playback

#### FR3: Signaling Protocol
**As a** developer  
**I want** a WebSocket-based signaling protocol  
**So that** peers can discover each other and exchange connection info

**Acceptance Criteria:**
- [ ] WebSocket connection to signaling server
- [ ] Room-based peer discovery (multiple peers can join same room)
- [ ] SDP offer/answer exchange
- [ ] ICE candidate exchange
- [ ] Proper error handling for signaling failures
- [ ] Reconnection logic if signaling disconnects

#### FR4: Data Channel Communication
**As a** developer  
**I want** reliable binary data transfer over WebRTC  
**So that** animation frames are delivered without corruption

**Acceptance Criteria:**
- [ ] Create data channel named "Open3DStream"
- [ ] Configure as reliable, ordered channel
- [ ] Handle binary messages containing O3DS FlatBuffer data
- [ ] Parse received data and extract SubjectList
- [ ] Forward to existing LiveLink animation pipeline
- [ ] Handle data channel close/error events

#### FR5: STUN/TURN Support
**As an** Unreal user  
**I want** NAT traversal to work automatically  
**So that** I can connect to remote peers behind firewalls

**Acceptance Criteria:**
- [ ] Configure default public STUN servers (e.g., `stun:stun.l.google.com:19302`)
- [ ] Allow custom STUN/TURN server configuration via URL parameters
- [ ] ICE candidate gathering works
- [ ] Connection succeeds through NAT
- [ ] Connection quality feedback (ICE state)

### Non-Functional Requirements

#### NFR1: Performance
- WebRTC connection establishment < 3 seconds
- Animation frame latency < 50ms
- CPU usage comparable to TCP/UDP protocols
- Memory usage < 50MB per connection

#### NFR2: Reliability
- Automatic reconnection on connection drop
- Graceful handling of network interruptions
- Proper cleanup of WebRTC resources
- No memory leaks in long-running sessions

#### NFR3: Compatibility
- Works on Windows, Linux, macOS (matching libdatachannel support)
- Compatible with UE 5.4, 5.5, 5.6+
- Interoperable with C++ command-line tools using libdatachannel
- Works with existing signaling server (`examples/signaling-server.js`)

#### NFR4: User Experience
- Clear error messages for common failures:
  - Signaling server unreachable
  - Peer not found in room
  - ICE connection timeout
  - STUN/TURN authentication failure
- Connection status visible in LiveLink UI
- Logging for debugging connection issues

## Technical Design

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│ Unreal LiveLink UI                                      │
│ (SOpen3DStreamFactory)                                  │
└────────────────┬────────────────────────────────────────┘
                 │ User selects "WebRTC Client"
                 │ URL: webrtc://server:8080/room
                 ▼
┌─────────────────────────────────────────────────────────┐
│ Open3DStreamSource                                      │
│ (Creates connector based on protocol)                   │
└────────────────┬────────────────────────────────────────┘
                 │
                 ▼
┌─────────────────────────────────────────────────────────┐
│ WebRTCConnector (FWebRTCConnector)                     │
│ - Manages libdatachannel lifecycle                      │
│ - Handles signaling via WebSocket                       │
│ - Creates rtc::PeerConnection                           │
│ - Manages data channels                                 │
└────────────────┬────────────────────────────────────────┘
                 │
      ┌──────────┼──────────┐
      ▼          ▼           ▼
┌──────────┐ ┌─────────┐ ┌──────────────┐
│Signaling │ │libdata  │ │Data Channel  │
│(WebSocket│ │channel  │ │Handler       │
│Client)   │ │Peer     │ │(Binary Data) │
└──────────┘ │Connection│ └──────────────┘
             └─────────┘
```

### Class Structure

#### 1. WebRTCConnector (Modify Existing)

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTCConnector.cpp`

**Key Changes:**
```cpp
#include <rtc/rtc.hpp>  // Include libdatachannel

class FWebRTCConnector : public IConnector
{
private:
    // libdatachannel objects
    std::shared_ptr<rtc::PeerConnection> PeerConnection;
    std::shared_ptr<rtc::DataChannel> DataChannel;
    std::shared_ptr<rtc::Configuration> RtcConfig;
    
    // Signaling
    TUniquePtr<class FWebRTCSignalingClient> SignalingClient;
    FString RoomName;
    FString SignalingServerUrl;
    
    // Callbacks for libdatachannel events
    void OnPeerConnectionStateChange(rtc::PeerConnection::State state);
    void OnDataChannelOpen();
    void OnDataChannelMessage(rtc::binary message);
    void OnDataChannelClosed();
    void OnIceCandidate(rtc::Candidate candidate);
    void OnLocalDescription(rtc::Description desc);
    
public:
    virtual bool Start(const FString& Url, bool bInIsServer) override;
    virtual void Stop() override;
    virtual bool Send(const uint8* Data, int32 Size) override;
    // ...
};
```

#### 2. WebRTCSignalingClient (New Class)

**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTCSignalingClient.h`

**Purpose**: Handle WebSocket communication with signaling server

```cpp
class FWebRTCSignalingClient
{
public:
    // Connect to signaling server
    bool Connect(const FString& Url, const FString& RoomName, bool bIsServer);
    
    // Send signaling messages
    void SendOffer(const FString& SDP);
    void SendAnswer(const FString& SDP);
    void SendIceCandidate(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex);
    
    // Callbacks for received messages
    TFunction<void(const FString& SDP)> OnOfferReceived;
    TFunction<void(const FString& SDP)> OnAnswerReceived;
    TFunction<void(const FString& Candidate, const FString& SdpMid, int32 SdpMLineIndex)> OnIceCandidateReceived;
    TFunction<void()> OnPeerJoined;
    TFunction<void()> OnPeerLeft;
    
private:
    TSharedPtr<IWebSocket> WebSocket;
    void OnWebSocketConnected();
    void OnWebSocketMessage(const FString& Message);
    void OnWebSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
};
```

### Implementation Steps

#### Phase 1: Basic Connection (Priority: High)
1. **Update WebRTCConnector.cpp**
   - Include libdatachannel headers
   - Create `rtc::Configuration` with default STUN servers
   - Implement `rtc::PeerConnection` creation
   - Add basic error handling

2. **Implement WebRTCSignalingClient**
   - WebSocket connection to signaling server
   - JSON message parsing for signaling protocol
   - Room join/leave logic
   - Peer discovery

3. **Implement Offer/Answer Flow**
   - Client creates offer
   - Server creates answer
   - SDP exchange via signaling
   - ICE candidate exchange

4. **Test Basic Connection**
   - Verify peer connection establishment
   - Verify ICE connection succeeds
   - Log connection state changes

#### Phase 2: Data Channel (Priority: High)
1. **Create Data Channel**
   - Configure as reliable, ordered
   - Set label to "Open3DStream"
   - Handle open/close events

2. **Implement Binary Data Handling**
   - Receive binary messages
   - Parse FlatBuffer data (existing code)
   - Forward to LiveLink pipeline

3. **Implement Send Functionality**
   - Serialize SubjectList to binary
   - Send via data channel
   - Handle send errors

4. **Test Data Transfer**
   - Verify animation frames received
   - Verify LiveLink displays animation
   - Test with Maya/MotionBuilder senders

#### Phase 3: Robustness (Priority: Medium)
1. **Error Handling**
   - Signaling connection failures
   - ICE connection timeout
   - Data channel errors
   - Proper error messages in UI

2. **Reconnection Logic**
   - Detect connection loss
   - Automatic reconnection attempts
   - Exponential backoff
   - User notification

3. **Resource Cleanup**
   - Proper PeerConnection disposal
   - Data channel cleanup
   - WebSocket closure
   - Memory leak testing

4. **Testing**
   - Connection stress tests
   - Network interruption simulation
   - Memory profiling
   - Performance benchmarking

#### Phase 4: Advanced Features (Priority: Low)
1. **Custom STUN/TURN**
   - Parse URL parameters for custom servers
   - TURN authentication
   - Multiple STUN servers

2. **Connection Quality**
   - ICE state monitoring
   - Connection quality metrics
   - Display in UI

3. **Multiple Peers**
   - Support multiple peers in same room
   - Mesh or star topology
   - Peer selection logic

### URL Format

```
webrtc://[signaling-host]:[port]/[room-name]?stun=[stun-server]&turn=[turn-server]

Examples:
webrtc://localhost:8080/myroom
webrtc://signal.example.com:8080/motion-capture
webrtc://10.0.1.100:8080/studio?stun=stun:stun.l.google.com:19302
webrtc://signal.example.com:8080/room?turn=turn:user:pass@turn.example.com:3478
```

**Parsing Logic:**
- Protocol: `webrtc://`
- Host/Port: Signaling server address
- Path: Room name (first segment)
- Query params: Optional STUN/TURN configuration

### Signaling Protocol (Existing)

The existing `examples/signaling-server.js` implements:

```json
// Join room
{ "type": "join", "room": "myroom", "name": "unreal-client" }

// Offer (from client)
{ "type": "offer", "sdp": "v=0\r\n..." }

// Answer (from server)
{ "type": "answer", "sdp": "v=0\r\n..." }

// ICE candidate
{ "type": "ice", "candidate": "...", "sdpMid": "0", "sdpMLineIndex": 0 }

// Peer events
{ "type": "peer-joined" }
{ "type": "peer-left" }
```

**No changes needed to signaling server** - it already supports the protocol.

## Testing Plan

### Unit Tests
- [ ] WebRTCConnector creation/destruction
- [ ] URL parsing logic
- [ ] Configuration object creation
- [ ] Error handling paths

### Integration Tests
- [ ] Connection to signaling server
- [ ] SDP offer/answer exchange
- [ ] ICE candidate gathering
- [ ] Data channel creation
- [ ] Binary data transmission

### End-to-End Tests
- [ ] Unreal client ↔ C++ server
- [ ] Unreal server ↔ C++ client
- [ ] Unreal ↔ Maya plugin
- [ ] Unreal ↔ MotionBuilder plugin
- [ ] Connection through NAT
- [ ] Reconnection after network interruption

### Performance Tests
- [ ] Connection establishment time
- [ ] Animation frame latency
- [ ] CPU usage profiling
- [ ] Memory usage profiling
- [ ] Long-running session stability

## Dependencies

### Completed (Issue #13)
- ✅ libdatachannel 0.23.2 libraries
- ✅ MbedTLS 3.6.5 with DTLS-SRTP
- ✅ Static linking configuration
- ✅ CI build workflow
- ✅ Pre-built libraries in repo

### Required for This Issue
- ✅ Unreal WebSockets module (already in Build.cs)
- ✅ JSON parsing (JsonUtilities already in Build.cs)
- ✅ Signaling server (already exists in `examples/signaling-server.js`)
- ⏳ Developer time to implement
- ⏳ Testing with real animation sources

## Documentation Updates

After implementation:

1. **README.md**
   - Update "WebRTC Protocol" section to show functional status
   - Update protocol comparison table
   - Add WebRTC setup instructions

2. **WEBRTC_QUICKSTART.md**
   - Remove warning about non-functional Unreal support
   - Add Unreal-specific setup steps
   - Add troubleshooting section

3. **New: WEBRTC_UNREAL_GUIDE.md**
   - Comprehensive Unreal WebRTC setup
   - STUN/TURN configuration
   - Firewall/NAT considerations
   - Performance tuning tips

4. **Plugin UI Help Text**
   - Add tooltip for WebRTC protocols
   - Link to documentation
   - Example URLs

## Success Criteria

### Minimum Viable Product (MVP)
- [ ] User can select "WebRTC Client" in Unreal
- [ ] Connection to signaling server succeeds
- [ ] Peer connection established with remote peer
- [ ] Animation data received and displayed in LiveLink
- [ ] Basic error messages for common failures

### Full Implementation
- [ ] Both client and server modes work
- [ ] Automatic reconnection on disconnect
- [ ] Custom STUN/TURN configuration
- [ ] Connection quality monitoring
- [ ] Comprehensive error handling
- [ ] Full test suite passing
- [ ] Documentation complete

## Estimated Effort

- **Research & Design**: 1 day (this document)
- **Phase 1 (Basic Connection)**: 2-3 days
- **Phase 2 (Data Channel)**: 1-2 days
- **Phase 3 (Robustness)**: 2-3 days
- **Phase 4 (Advanced Features)**: 1-2 days
- **Testing & Documentation**: 1-2 days

**Total**: 8-13 days of development work

## Related Issues

- **Issue #13**: Integrate libdatachannel with MbedTLS (COMPLETE)
  - Provides the foundation (libraries, build system)
  - This issue builds on top of that foundation

## References

- libdatachannel API: https://github.com/paullouisageneau/libdatachannel
- libdatachannel examples: https://github.com/paullouisageneau/libdatachannel/tree/master/examples
- Existing signaling server: `examples/signaling-server.js`
- Current stub implementation: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTCConnector.cpp`

## Notes

This issue focuses on **implementation**, not build infrastructure. Issue #13 completed the build infrastructure, ensuring libdatachannel can be linked and used. This issue is about actually using the libdatachannel API to create functional WebRTC connections.
