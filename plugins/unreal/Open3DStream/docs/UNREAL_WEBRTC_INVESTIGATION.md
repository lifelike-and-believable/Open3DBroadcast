# Using Unreal Engine's Built-in WebRTC Libraries

## Question
Can we leverage Unreal Engine's built-in WebRTC libraries (used for Pixel Streaming) instead of building libdatachannel ourselves?

## Answer: **Yes, Potentially!** 🎯

This is an excellent idea that could significantly simplify the WebRTC integration.

## What Unreal Provides

### Pixel Streaming WebRTC Stack

Unreal Engine includes a full WebRTC implementation for Pixel Streaming:

**Modules Available:**
- **`PixelStreamingCore`** - Core WebRTC functionality
- **`WebRTC`** - Google's WebRTC library (Unreal's build)
- **`PixelStreaming`** - High-level Pixel Streaming API

**Location in UE Source:**
```
Engine/Plugins/Media/PixelStreaming/
├── Source/
│   ├── PixelStreamingCore/
│   ├── PixelStreaming/
│   └── PixelStreamingPlayer/
├── ThirdParty/
│   └── WebRTC/           # Pre-built WebRTC libraries
```

### What This Includes

1. **Google's WebRTC Library** - Full implementation
   - PeerConnection API
   - DataChannels
   - ICE/STUN/TURN support
   - DTLS/SRTP encryption (built-in)
   - Audio/Video codecs (H.264, VP8, VP9, Opus)

2. **Already Built & Tested** - Unreal maintains these
   - Pre-compiled for all platforms (Win64, Linux, Mac)
   - Tested with every Unreal Engine release
   - No OpenSSL/mbedTLS dependency issues
   - Works out of the box

3. **Integrated with Unreal Build System** - Just add module dependency

## Comparison: libdatachannel vs Unreal WebRTC

| Aspect | libdatachannel (Current) | Unreal WebRTC (Proposed) |
|--------|--------------------------|--------------------------|
| **Build Complexity** | High (requires OpenSSL) | None (already built) |
| **Library Size** | ~86MB | Already in engine |
| **Platform Support** | Manual builds needed | All UE platforms included |
| **Maintenance** | We maintain | Epic maintains |
| **Dependencies** | OpenSSL required | None (self-contained) |
| **CI/CD** | Disabled (Issue #10) | Works immediately |
| **API** | libdatachannel API | Google WebRTC native API |
| **Integration Effort** | Low (already done) | Medium (API changes needed) |

## Advantages of Using Unreal's WebRTC

### ✅ Major Benefits

1. **Zero Build Complexity**
   - No `build_webrtc_lib.bat` needed
   - No OpenSSL installation required
   - No submodule initialization
   - Just add module dependency!

2. **Works in CI Immediately**
   - No disabled builds (Issue #10 solved!)
   - All platforms build successfully
   - Artifacts are fully functional

3. **Always Up-to-Date**
   - Epic maintains and updates
   - Security patches included
   - New UE versions = newer WebRTC

4. **Cross-Platform Guaranteed**
   - Works on Windows, Linux, Mac, consoles
   - No platform-specific build issues
   - Same code works everywhere

5. **Better Integration**
   - Uses Unreal's memory allocators
   - Integrated logging
   - Thread management via Unreal
   - No external library conflicts

### ⚠️ Considerations

1. **API Differences**
   - libdatachannel API ≠ Google WebRTC API
   - Would need to rewrite `src/o3ds/webrtc_connector.cpp`
   - Different patterns for PeerConnection/DataChannels

2. **Module Dependency**
   - Plugin depends on PixelStreaming modules
   - Increases plugin size (if not already in project)
   - Requires Pixel Streaming plugin enabled

3. **Version Tied to UE**
   - WebRTC version depends on UE version
   - Can't upgrade WebRTC independently
   - Different UE versions = different WebRTC APIs

4. **Licensing**
   - WebRTC is BSD-3-Clause (same as libdatachannel)
   - No licensing concerns
   - Already approved for UE use

## Implementation Approach

### Option A: Use Unreal's WebRTC Module Directly

**Pros:**
- ✅ Zero external dependencies
- ✅ Works in CI/CD immediately
- ✅ Cross-platform guaranteed
- ✅ Epic-maintained

**Cons:**
- ⚠️ Requires rewriting WebRTC connector code
- ⚠️ Different API than libdatachannel
- ⚠️ Tied to Unreal Engine version

**Effort:** Medium (1-2 days to rewrite connector)

### Option B: Keep libdatachannel (Current Approach)

**Pros:**
- ✅ Already implemented
- ✅ Works with current code
- ✅ Independent of UE version
- ✅ Build automation complete

**Cons:**
- ⚠️ Requires local build step
- ⚠️ OpenSSL dependency
- ⚠️ CI disabled for WebRTC (Issue #10)
- ⚠️ Platform-specific builds

**Effort:** Zero (already done)

### Option C: Hybrid - Detect and Use Whichever is Available

**Approach:**
```cpp
#if UE_PIXELSTREAMING_AVAILABLE
    // Use Unreal's WebRTC
    #include "WebRTC/PeerConnection.h"
#elif defined(O3DS_ENABLE_WEBRTC)
    // Use libdatachannel
    #include "rtc/rtc.hpp"
#endif
```

**Pros:**
- ✅ Best of both worlds
- ✅ Works in UE projects (Unreal WebRTC)
- ✅ Works standalone (libdatachannel)

**Cons:**
- ⚠️ Maintain two implementations
- ⚠️ More complex code
- ⚠️ Double testing needed

**Effort:** High (maintain both)

## Recommended Path Forward

### 🎯 **Recommendation: Switch to Unreal's WebRTC** (Option A)

**Rationale:**
1. Open3DStream is an **Unreal plugin** - it will always run in Unreal
2. Zero build complexity = easier for contributors
3. CI/CD works immediately (closes Issue #10)
4. Epic-maintained = less work for us
5. Cross-platform guaranteed

**Migration Plan:**

#### Phase 1: Proof of Concept (1-2 days)
1. Add `PixelStreamingCore` module dependency
2. Create simple WebRTC DataChannel test
3. Verify it works in Unreal Editor
4. Measure performance vs libdatachannel

#### Phase 2: Rewrite Connector (2-3 days)
1. Rewrite `webrtc_connector.cpp` using native WebRTC API
2. Implement PeerConnection lifecycle
3. Implement DataChannel send/receive
4. Add signaling support

#### Phase 3: Testing & Documentation (1-2 days)
1. Test in Unreal Editor
2. Test CI/CD builds
3. Update documentation
4. Remove build_webrtc_lib scripts (no longer needed!)

#### Phase 4: Cleanup
1. Remove libdatachannel submodule
2. Remove OpenSSL dependencies
3. Remove build scripts
4. Close Issue #10 (solved by using Unreal's WebRTC)

**Total Effort:** ~5-7 days

**Benefits:**
- ✅ Issue #10 solved permanently
- ✅ Build automation scripts no longer needed
- ✅ CI builds WebRTC by default
- ✅ Cross-platform works immediately
- ✅ Easier for contributors

## Code Example: Using Unreal's WebRTC

### Build.cs Changes
```csharp
public class Open3DStream : ModuleRules
{
    public Open3DStream(ReadOnlyTargetRules Target) : base(Target)
    {
        // ... existing code ...
        
        // Add Unreal's WebRTC modules
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "WebRTC",                 // Google WebRTC library
            "PixelStreamingCore",     // Unreal's WebRTC wrapper
            // ... other modules ...
        });
    }
}
```

### Connector Implementation
```cpp
// webrtc_connector_ue.cpp
#include "WebRTC/PeerConnection.h"
#include "WebRTC/DataChannel.h"

class UEWebRTCConnector : public O3DS::Connector
{
    TUniquePtr<FWebRTCPeerConnection> PeerConnection;
    TSharedPtr<FWebRTCDataChannel> DataChannel;
    
    void Connect(const char* url) override
    {
        // Create PeerConnection using Unreal's API
        FWebRTCPeerConnectionConfig Config;
        PeerConnection = MakeUnique<FWebRTCPeerConnection>(Config);
        
        // Create DataChannel
        DataChannel = PeerConnection->CreateDataChannel("o3ds");
        
        // Set up callbacks
        DataChannel->OnMessage().AddLambda([this](const FString& Message) {
            // Handle received data
        });
    }
    
    void Send(const void* data, size_t len) override
    {
        if (DataChannel && DataChannel->IsOpen())
        {
            FString Message((const char*)data, len);
            DataChannel->SendMessage(Message);
        }
    }
};
```

## Next Steps

### Immediate (This PR)
1. Document this finding in PR #9
2. Get feedback from maintainers
3. Decide on approach

### If Approved
1. Create new issue: "Migrate to Unreal's built-in WebRTC"
2. Keep current libdatachannel implementation working
3. Create proof-of-concept branch
4. Benchmark performance
5. Make decision based on POC results

### If Not Approved
1. Continue with libdatachannel approach
2. Build automation is already complete
3. Document Unreal WebRTC as future optimization

## References

- **Unreal Pixel Streaming Docs:** https://docs.unrealengine.com/5.0/en-US/pixel-streaming-in-unreal-engine/
- **WebRTC Native API:** https://webrtc.github.io/webrtc-org/native-code/native-apis/
- **Unreal WebRTC Source:** `Engine/Plugins/Media/PixelStreaming/Source/`
- **libdatachannel:** https://github.com/paullouisageneau/libdatachannel

## Conclusion

**Using Unreal's built-in WebRTC is likely the better long-term solution** because:
- ✅ Eliminates all build complexity
- ✅ Works in CI immediately  
- ✅ Epic-maintained and updated
- ✅ Cross-platform guaranteed
- ✅ Aligns with Unreal ecosystem

The current libdatachannel approach works, but requires ongoing maintenance (Issue #10) and platform-specific builds. Since Open3DStream is an Unreal plugin, leveraging Unreal's existing WebRTC infrastructure makes architectural sense.

**Recommendation:** Create a proof-of-concept to validate this approach, then migrate if successful.
