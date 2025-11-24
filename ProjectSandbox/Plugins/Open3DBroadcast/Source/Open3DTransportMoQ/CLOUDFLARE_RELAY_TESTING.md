# CloudFlare Relay Testing Guide

This guide explains how to run the MoQ transport tests against CloudFlare's public MoQ relay network.

## Overview

The MoQ Transport module includes integration tests that verify connectivity and functionality with CloudFlare's experimental MoQ relay at `https://relay.cloudflare.mediaoverquic.com`.

## Test Suite

The following tests are available in the `Open3DBroadcast.Open3DTransportMoQ.Cloudflare` test group:

1. **Connectivity** - Verifies basic connection to CloudFlare relay
2. **Publish** - Tests track announcement and publisher creation
3. **Sender** - Tests high-level FO3DMoQSender API with CloudFlare relay
4. **MultiPublisher** - Stress test with multiple concurrent publishers

## Prerequisites

- **Internet connectivity** - Tests require access to CloudFlare's relay network
- **O3D_WITH_TRANSPORT_MOQ=1** - MoQ transport must be enabled (default)
- **moq_ffi.dll** - Must be present in `ThirdParty/moq-ffi/bin/Win64/Release/`

## Running Tests in Unreal Editor

### Method 1: Session Frontend (Recommended)

1. Open `ProjectSandbox.uproject` in Unreal Editor
2. Open **Window → Test Automation** (or **Window → Developer Tools → Session Frontend**)
3. In the Session Frontend, switch to the **Automation** tab
4. In the filter box, type: `Open3DBroadcast.Open3DTransportMoQ.Cloudflare`
5. Check the boxes next to the tests you want to run
6. Click **Start Tests**
7. Monitor the output panel for results

### Method 2: Console Commands

1. Open `ProjectSandbox.uproject` in Unreal Editor  
2. Open the Output Log (Window → Developer Tools → Output Log)
3. In the console, run:
   ```
   Automation RunTests Open3DBroadcast.Open3DTransportMoQ.Cloudflare.Connectivity
   ```
4. Check the log for test results

### Method 3: Blueprints/Code

You can also programmatically trigger tests:

```cpp
#include "AutomationBlueprintFunctionLibrary.h"

// Run all CloudFlare relay tests
UAutomationBlueprintFunctionLibrary::StartAutomationTests(
    "Open3DBroadcast.Open3DTransportMoQ.Cloudflare"
);
```

## Running Tests from Command Line

⚠️ **Known Limitation:** Command-line automation tests currently fail because the `moq_ffi.dll` uses delay-loading and the module may not initialize before tests run. Use the in-editor methods above instead.

If you still want to try command-line testing (for CI/CD integration), ensure the module loads first by:

```powershell
# This approach doesn't currently work reliably - use editor instead
& 'c:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
    'E:\OtherProjects\Open3DStream\ProjectSandbox\ProjectSandbox.uproject' `
    -ExecCmds="Automation RunTests Open3DBroadcast.Open3DTransportMoQ.Cloudflare; Quit" `
    -Unattended -NullRHI -Log
```

## Expected Test Behavior

### Test: Connectivity

**What it does:**
- Initializes a session with CloudFlare relay URL
- Attempts WebTransport connection
- Waits up to 10 seconds for connection establishment
- Reports connection state changes

**Expected output:**
```
Testing connectivity to CloudFlare MoQ relay at: https://relay.cloudflare.mediaoverquic.com
Session initialization should succeed: true
Attempting to connect to CloudFlare relay...
Connection result: SUCCESS (state: Connected)
```

**Possible outcomes:**
- ✅ **Success** - Connected to relay within timeout
- ⚠️ **Timeout** - No connection after 10 seconds (check internet connectivity)
- ❌ **Error** - FFI library not loaded or configuration issue

### Test: Publish

**What it does:**
- Connects to CloudFlare relay
- Announces a unique test namespace (e.g., `mocap/test_1732300000`)
- Creates a publisher for a character track
- Generates and attempts to publish a minimal test payload

**Expected output:**
```
Testing track announcement and publish to CloudFlare relay
Using test namespace: mocap/test_1732300000
Namespace announcement should succeed: true
Publisher creation should succeed: true
Publisher handle should be valid: true
Publishing test payload (123 bytes)
```

### Test: Sender

**What it does:**
- Tests the high-level `FO3DMoQSender` API
- Initializes sender with CloudFlare relay URL
- Starts connection (async)
- Waits for connection with periodic ticking
- Reports final statistics

**Expected output:**
```
Testing FO3DMoQSender with CloudFlare relay
Using stream ID: test/sender_1732300000
Sender initialization should succeed: true
Sender start should succeed: true
Sender started, waiting for connection...
Sender appears operational (no immediate errors)
Final stats - BytesSent: 0, FramesSent: 0
```

### Test: MultiPublisher

**What it does:**
- Creates 3 concurrent sessions to CloudFlare relay
- Each session uses a unique namespace
- Verifies all publishers can coexist
- Tests namespace isolation

**Expected output:**
```
Testing multiple concurrent publishers to CloudFlare relay
Publisher 0 created successfully
Publisher 1 created successfully
Publisher 2 created successfully
Should create all publishers: true (3 == 3)
Multi-publisher test completed with 3 concurrent publishers
```

## Troubleshooting

### "MoQ FFI library not loaded"

**Problem:** The moq_ffi.dll is not available or not loaded by the module.

**Solutions:**
1. Check that `ThirdParty/moq-ffi/bin/Win64/Release/moq_ffi.dll` exists
2. Verify `O3D_WITH_TRANSPORT_MOQ=1` (should be default)
3. Rebuild the plugin
4. Check Output Log for module startup errors:
   ```
   LogO3DMoQSender: Open3D MoQ transport module starting...
   LogMoQFfiSupport: Successfully loaded MoQ FFI library from: ...
   ```

### "Connection timeout"

**Problem:** Cannot connect to CloudFlare relay within 10 seconds.

**Solutions:**
1. Verify internet connectivity
2. Check if CloudFlare relay is operational (try `curl https://relay.cloudflare.mediaoverquic.com`)
3. Check firewall settings (allow QUIC/UDP traffic on port 443)
4. Try increasing timeout in test code if needed

### "Initialization failed"

**Problem:** Session initialization fails before connection attempt.

**Solutions:**
1. Check relay URL is correct: `https://relay.cloudflare.mediaoverquic.com`
2. Verify FFI library exports expected functions
3. Check logs for detailed error messages

### "Publisher creation failed"

**Problem:** Cannot create publisher after successful connection.

**Solutions:**
1. Check namespace format (should be `mocap/session/name`)
2. Verify track name is valid (alphanumeric + underscore/dash)
3. Check if namespace was announced before publisher creation
4. Review CloudFlare relay logs (if accessible)

## Performance Expectations

Based on internet connectivity, expect:

- **Connection time:** 1-5 seconds (depends on latency to CloudFlare)
- **Namespace announcement:** <500ms after connection
- **Publisher creation:** <200ms
- **Object publish:** <100ms per object

Latency will vary based on geographic location and network conditions.

## CloudFlare Relay Details

**Relay URL:** `https://relay.cloudflare.mediaoverquic.com`

**Protocol:** IETF draft-ietf-moq-transport-07 (via moq-rs)

**Transport:** WebTransport over QUIC v1 (RFC 9000) with TLS 1.3

**Features:**
- Track-based pub/sub routing
- Namespace announcements
- Multiple concurrent publishers/subscribers
- N:M fanout capability

**Limitations:**
- Experimental network - availability not guaranteed
- May have rate limits or usage restrictions
- Not suitable for production use (yet)
- No SLA or reliability guarantees

## Next Steps

After verifying CloudFlare relay connectivity:

1. **Phase 3:** Implement receiver (`FO3DMoQReceiver`) with subscription support
2. **Phase 4:** Add audio track support for multi-track scenarios
3. **Phase 5:** Deploy local `moq-relay-ietf` for development/testing
4. **Production:** Evaluate dedicated relay deployment vs. CloudFlare network

## References

- **Implementation Plan:** `MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md`
- **FFI Layer:** `ThirdParty/moq-ffi/README.md`
- **Build Configuration:** `O3D_WITH_TRANSPORT_MOQ.md`
- **CloudFlare MoQ:** https://blog.cloudflare.com/media-over-quic
- **IETF MoQ Spec:** https://datatracker.ietf.org/doc/draft-ietf-moq-transport/

## Support

For issues with CloudFlare relay tests:

1. Check this guide for troubleshooting steps
2. Review Output Log for detailed error messages
3. File issue in Open3DStream repository with:
   - Test name and expected vs actual behavior
   - Full log output from test run
   - Internet connectivity status
   - Platform and build configuration

---

**Note:** These tests validate Phase 2 sender implementation. Phase 3 receiver tests will be added once `FO3DMoQReceiver` is implemented.
