# Plugin Capabilities - Sanity Check (Issue #13 Post-Implementation)

**Date**: October 17, 2025  
**Context**: After completing Issue #13 (MbedTLS integration), verifying what end-users can actually do

---

## Executive Summary

⚠️ **CRITICAL FINDING**: The Unreal plugin includes **libdatachannel libraries but WebRTC functionality is NOT implemented**.

### What End-Users CAN Do ✅

1. **TCP Client** - Connect to TCP servers
2. **UDP Server** - Receive UDP packets  
3. **NNG Subscribe** - Subscribe to NNG publishers
4. **NNG Client/Server** - Bidirectional NNG communication
5. **Animation Curves** - Receive morph target data alongside skeletal data

### What End-Users CANNOT Do ❌

1. **WebRTC Client/Server** - Not implemented (stub only)
2. **WebSocket** - Build.cs includes WebSockets module, but no connector implementation found

---

## Detailed Analysis

### 1. What the Plugin Build Includes

From `Open3DStream.Build.cs`:

```csharp
// ✅ INCLUDED: Static libraries are linked
PublicAdditionalLibraries.Add(WebRTCDir + "datachannel.lib");      // Windows
PublicAdditionalLibraries.Add(WebRTCDir + "libdatachannel.a");     // Linux/Mac

// ✅ INCLUDED: Static mode is defined
PublicDefinitions.Add("RTC_STATIC=1");

// ✅ INCLUDED: Dependencies declared
PrivateDependencyModuleNames.AddRange(
    new string[] {
        "PixelStreaming",   // For WebRTC integration
        "WebRTC",           // Unreal's WebRTC module
        "WebSockets",       // For WebSocket protocol
        // ...
    }
);
```

### 2. What the UI Shows

From `SOpen3DStreamFactory.cpp`:

```cpp
// Protocol dropdown options in LiveLink source creation dialog:
Options.Add(MakeShareable(new FString("NNG Subscribe (to NNG Publish)")));     // ✅ Works
Options.Add(MakeShareable(new FString("NNG Client (to NNG Server)")));         // ✅ Works
Options.Add(MakeShareable(new FString("NNG Server (to NNG Client)")));         // ✅ Works
Options.Add(MakeShareable(new FString("TCP Client")));                          // ✅ Works
Options.Add(MakeShareable(new FString("UDP Server")));                          // ✅ Works
Options.Add(MakeShareable(new FString("WebRTC Client")));                       // ❌ NOT IMPLEMENTED
Options.Add(MakeShareable(new FString("WebRTC Server")));                       // ❌ NOT IMPLEMENTED
```

**Problem**: Users can SELECT "WebRTC Client" or "WebRTC Server" from the dropdown, but it will fail with an error message.

### 3. What the WebRTC Connector Actually Does

From `WebRTCConnector.cpp`:

```cpp
bool FWebRTCConnector::Start(const FString& Url, bool bInIsServer)
{
    // TODO: Implement Pixel Streaming WebRTC integration
    // ...
    
    UE_LOG(LogTemp, Warning, TEXT("WebRTC Connector: Implementation incomplete - WebRTC support requires Pixel Streaming API integration"));
    UE_LOG(LogTemp, Warning, TEXT("WebRTC was requested for URL: %s (Mode: %s)"), *Url, bInIsServer ? TEXT("Server") : TEXT("Client"));
    
    LastError = TEXT("WebRTC support via Pixel Streaming is not yet implemented. Please use TCP or WebSocket protocols.");
    return false;  // ❌ Always returns false
}

bool FWebRTCConnector::Send(const uint8* Data, int32 Size)
{
    LastError = TEXT("WebRTC Send not yet implemented");
    return false;  // ❌ Always returns false
}
```

**Status**: The WebRTC connector is a **stub implementation** with TODO comments. It logs warnings and returns failure.

### 4. What the Documentation Claims

#### README.md says:

> ### WebRTC Protocol (October 2025)
> Implemented WebRTC as a network transport for peer-to-peer streaming:
> - Low-latency peer-to-peer connections
> - Built-in NAT traversal (STUN/TURN)
> - DTLS encryption
> - Room-based peer grouping
> - See [WEBRTC_QUICKSTART.md](WEBRTC_QUICKSTART.md) for 5-minute setup

#### Protocol Comparison table shows:

| Protocol | Best For |
|----------|----------|
| **WebRTC** | **✅ Cloud, remote** |

**Reality**: ❌ WebRTC does NOT work in the Unreal plugin despite the documentation.

---

## Issue #13 Scope vs. Reality

### What Issue #13 Accomplished ✅

1. **CI Workflow**: Created `.github/workflows/build-libdatachannel.yml` for building libdatachannel with MbedTLS
2. **Libraries Built**: Successfully built static libraries for all platforms (Windows, Linux, macOS)
3. **Libraries Integrated**: Committed pre-built libraries to `plugins/unreal/Open3DStream/ThirdParty/webrtc/`
4. **Build Configuration**: Updated `Build.cs` to link libdatachannel statically with `RTC_STATIC=1`
5. **Documentation**: Created comprehensive integration guides

### What Issue #13 Did NOT Accomplish ❌

1. **WebRTC Implementation**: Did not implement actual WebRTC functionality in the Unreal plugin
2. **libdatachannel Usage**: Libraries are linked but not actually used by any code
3. **API Integration**: No integration between libdatachannel C++ API and Unreal's connector classes

**Conclusion**: Issue #13 was about **build infrastructure**, not **functionality implementation**.

---

## What End-Users Should Be Told

### Working Features (Install & Use Immediately)

**Installation:**
1. Download the latest release from GitHub
2. Extract to `YourProject/Plugins/Open3DStream/`
3. Enable plugin in Unreal Editor: **Edit → Plugins** → "Open3DStream"
4. Restart Unreal Editor

**Usage - TCP Client (Recommended):**
1. Open **Window → Virtual Production → Live Link**
2. Click **+ Source → Open3DStream Source**
3. Configure:
   - **URL**: `tcp://localhost:5555`
   - **Protocol**: Select **"TCP Client"**
4. Click **Create**

**Supported Protocols:**
- ✅ **TCP Client** - Connects to remote/local TCP servers
- ✅ **UDP Server** - Receives UDP broadcast data
- ✅ **NNG Subscribe** - Subscribes to NNG publisher endpoints
- ✅ **NNG Client** - Connects to NNG server endpoints
- ✅ **NNG Server** - Accepts NNG client connections

**Data Support:**
- ✅ Skeletal animation (transforms, rotations, scales)
- ✅ Animation curves (morph targets for facial animation)
- ✅ Multiple subjects per stream
- ✅ Delta-optimized updates

### Non-Working Features (Do Not Use)

❌ **WebRTC Client/Server** - Listed in UI but not implemented
- Will show error: "WebRTC support via Pixel Streaming is not yet implemented"
- Recommendation: Use TCP for local networks, or set up VPN for remote access

❌ **WebSocket** - Module linked but no connector implementation
- No WebSocket option appears in UI (correctly hidden)
- Use TCP as alternative

---

## Recommended Documentation Updates

### 1. README.md

**Remove or Update "WebRTC Protocol" section:**

```markdown
### WebRTC Protocol (October 2025)
Implemented WebRTC as a network transport for peer-to-peer streaming:  ❌ INCORRECT
```

**Replace with:**

```markdown
### WebRTC Build Infrastructure (October 2025)
Added build infrastructure for libdatachannel integration:
- Pre-built libdatachannel libraries with MbedTLS backend
- Automated CI workflow for building libraries
- Static linking configuration
- **Note**: WebRTC functionality is not yet implemented in the Unreal plugin. Use TCP or UDP protocols.
```

### 2. Protocol Comparison Table

**Current (Misleading):**
| Protocol | Best For |
|----------|----------|
| **WebRTC** | **✅ Cloud, remote** |

**Should Be:**
| Protocol | Status | Best For |
|----------|--------|----------|
| TCP | ✅ Implemented | Local networks, reliable delivery |
| UDP | ✅ Implemented | Low latency, lossy OK |
| NNG | ✅ Implemented | Microservices, IPC |
| WebRTC | ⏳ Build infrastructure only | Future: Cloud, remote |

### 3. WEBRTC_QUICKSTART.md

**Add prominent notice at the top:**

```markdown
# WebRTC Quick Start Guide

> ⚠️ **IMPORTANT**: This guide documents the **C++ command-line tools** and build infrastructure.
> WebRTC functionality is **NOT YET IMPLEMENTED** in the Unreal Engine plugin.
> 
> **For Unreal users**: Please use TCP, UDP, or NNG protocols. See main README.md for instructions.
```

### 4. Plugin UI

**Update `SOpen3DStreamFactory.cpp` to hide non-working protocols:**

```cpp
// Remove or comment out until implemented:
// Options.Add(MakeShareable(new FString("WebRTC Client")));
// Options.Add(MakeShareable(new FString("WebRTC Server")));
```

---

## Implementation Gap

### To Make WebRTC Actually Work

The following work is required:

1. **libdatachannel C++ Integration**:
   - Include libdatachannel headers: `#include <rtc/rtc.hpp>`
   - Create `rtc::Configuration` with STUN/TURN servers
   - Implement `rtc::PeerConnection` lifecycle
   - Create data channels with `pc->createDataChannel("Open3DStream")`
   - Handle WebRTC signaling (offer/answer/ICE)

2. **Signaling Implementation**:
   - Connect to WebSocket signaling server
   - Exchange SDP offers/answers between peers
   - Exchange ICE candidates
   - Handle room-based peer discovery

3. **Data Channel Callbacks**:
   - Implement `onOpen()` callback
   - Implement `onMessage()` to receive binary data
   - Implement `onClosed()` callback
   - Forward received data to Unreal's LiveLink system

4. **Remove Pixel Streaming Dependency**:
   - Current stub tries to use Pixel Streaming API
   - Should use libdatachannel directly instead
   - Remove Pixel Streaming module dependency

**Estimated Effort**: 3-5 days of development + testing

---

## Conclusion

### Issue #13 Status: ✅ Complete (Build Infrastructure)

- All CI builds passing
- Libraries integrated correctly
- Documentation (for build process) complete
- Ready to merge

### User-Facing WebRTC: ❌ Not Implemented

- WebRTC options should be hidden or marked as "Coming Soon"
- Documentation should be clarified to avoid confusion
- Separate issue should be created for actual implementation

### Recommendation

**Before merging PR #14**, consider:

1. **Option A: Update documentation to clarify current state**
   - Clearly state WebRTC is build infrastructure only
   - Remove claims of working WebRTC in Unreal plugin
   - Update protocol comparison table
   - ✅ Honest, prevents user confusion

2. **Option B: Hide WebRTC options in UI**
   - Comment out WebRTC protocol options
   - Keep code for future implementation
   - ✅ Prevents users from trying broken feature

3. **Option C: Keep as-is with clear error messages**
   - UI shows WebRTC options
   - Error message explains it's not implemented
   - Documentation updated
   - ✅ Shows feature is planned

**Recommended**: Combination of A + B - Update docs AND hide UI options until implemented.

---

## Files Requiring Updates

1. `README.md` - Clarify WebRTC status
2. `WEBRTC_QUICKSTART.md` - Add warning for Unreal users
3. `plugins/unreal/Open3DStream/Source/Open3DStream/Private/SOpen3DStreamFactory.cpp` - Hide WebRTC options
4. Create new issue: "Implement WebRTC functionality in Unreal plugin using libdatachannel"
