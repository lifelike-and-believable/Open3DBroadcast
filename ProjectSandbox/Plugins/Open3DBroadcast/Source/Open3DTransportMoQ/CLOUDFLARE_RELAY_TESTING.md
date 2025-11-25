# CloudFlare Relay Testing Guide

This guide documents the CloudFlare relay automation tests for the MoQ transport module.

> **Status (2025-11-24):** All Cloudflare relay tests now run as latent automation commands. They exercise real network paths, so expect occasional flakiness if the public relay is unavailable.

## Overview

The MoQ transport module ships a small automation suite that verifies connectivity against CloudFlare's experimental relay at `https://relay.cloudflare.mediaoverquic.com` (or an override supplied via `O3D_MOQ_RELAY_URL`). Each test now uses latent automation commands so the game thread never blocks while waiting for external network operations. These tests provide the positive-path coverage required before Phase 3 (receiver) work begins.

## Test Suite & Status

| Test | Automation Path | Status | Notes |
| --- | --- | --- | --- |
| Basic | `Open3DBroadcast.Open3DTransportMoQ.Cloudflare.Basic` | ✅ Enabled | Verifies `moq_ffi.dll` loads and a session can be initialized/disconnected without panic. |
| Connectivity | `...Cloudflare.Connectivity` | ✅ Enabled | Uses a session harness + latent wait to confirm relay handshake reaches `MOQ_STATE_CONNECTED`. |
| Publish | `...Cloudflare.Publish` | ✅ Enabled | Announces a unique namespace and publishes a serialized `O3DS::SubjectList` payload. |
| PublishSubscribe | `...Cloudflare.PublishSubscribe` | ✅ Enabled | Spins up publisher/subscriber harnesses and waits for a payload echo via `OnData`. |
| Sender | `...Cloudflare.Sender` | ✅ Enabled | Exercises `FO3DMoQSender` to send multiple frames and waits for stats to report progress. |
| MultiPublisher | `...Cloudflare.MultiPublisher` | ✅ Enabled | Creates three concurrent sessions, announces isolated namespaces, and validates each publisher handle. |

Tracking for any future regressions lives in `Phase3_Readiness_Report.md` (Phase 2 gaps 4 & 6).

## Prerequisites

- **Internet connectivity** - Tests require access to CloudFlare's relay network
- **O3D_WITH_TRANSPORT_MOQ=1** - MoQ transport must be enabled (default on Win64; auto-disabled elsewhere)
- **moq_ffi.dll** - Must be present in `ThirdParty/moq-ffi/bin/Win64/Release/` (verify SHA256 in `ThirdParty/moq-ffi/README.md`)
- **Relay override (optional)** - Set `O3D_MOQ_RELAY_URL` to point at a locally hosted `moq-relay-ietf` instance when the public relay is unavailable.

## Running Tests in Unreal Editor

### Method 1: Session Frontend (Recommended)

1. Open `ProjectSandbox.uproject` in Unreal Editor
2. Open **Window → Test Automation** (or **Window → Developer Tools → Session Frontend**)
3. In the Session Frontend, switch to the **Automation** tab
4. In the filter box, type: `Open3DBroadcast.Open3DTransportMoQ.Cloudflare`
5. Expand `Open3DBroadcast → Open3DTransportMoQ → Cloudflare`
6. Select the tests you want to run (running the full group takes ~2–3 minutes depending on network latency)
7. Click **Start Tests** and monitor the output panel for results

### Method 2: Console Commands

1. Open `ProjectSandbox.uproject` in Unreal Editor  
2. Open the Output Log (Window → Developer Tools → Output Log)
3. In the console, run either a single test or the full group:
   ```
   Automation RunTests Open3DBroadcast.Open3DTransportMoQ.Cloudflare.Publish
   Automation RunTests Open3DBroadcast.Open3DTransportMoQ.Cloudflare
   ```
4. Keep the Output Log visible—each latent step emits progress when timeouts occur or when payloads are received.

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

⚠️ **Known Limitations:**
- Command-line automation can still fail if the delay-loaded `moq_ffi.dll` initializes after the tests execute.
- Latent relay tests depend on live network traffic; unattended builds may need generous timeouts or a dedicated local relay to stay reliable.

If you still want to try command-line testing (for CI/CD integration), ensure the module loads first by:

```powershell
# This approach doesn't currently work reliably - use editor instead
& 'c:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
    'E:\OtherProjects\Open3DStream\ProjectSandbox\ProjectSandbox.uproject' `
    -ExecCmds="Automation RunTests Open3DBroadcast.Open3DTransportMoQ.Cloudflare; Quit" `
    -Unattended -NullRHI -Log
```

## Expected Test Behavior

### Test: Basic

**Purpose:** Smoke-test the FFI plumbing by verifying that `moq_ffi.dll` loads, the required exports exist, and `moq_connect` can be invoked without crashing.

**What to expect:**
- Logs confirming the DLL path, version (if reported), and connection attempt
- The test completes once `moq_connect` returns (it does not wait for an actual relay connection yet)
- Any missing dependency immediately surfaces as a failed assertion with guidance to check `ThirdParty/moq-ffi/README.md`

### Test: Connectivity

**What it does:**
- Initializes a session with the CloudFlare relay URL (or `O3D_MOQ_RELAY_URL` override)
- Adds a connection-state delegate and calls `Session->Connect()`
- Uses a latent command (`FWaitForMoQStateCommand`) to wait up to 15 seconds for `MOQ_STATE_CONNECTED`

**Expected output:**
```
Connecting to Cloudflare MoQ relay at https://relay.cloudflare.mediaoverquic.com
Connectivity: session initializes: true
Connectivity: connect call succeeds: true
Waiting for Cloudflare connectivity... (timeout 15s)
```

**Possible outcomes:**
- ✅ **Success** - Reaches `MOQ_STATE_CONNECTED` before the timeout
- ⚠️ **Timeout** - No connection within 15 seconds (often due to relay maintenance or blocked QUIC traffic)
- ❌ **Error** - `FMoQFfiSupport::IsLoaded()` fails or the relay URL is incorrect

### Test: Publish

**What it does:**
- Waits for a connected session (same harness as Connectivity)
- Announces a GUID-backed namespace to avoid collisions across runs
- Creates a publisher handle with `MOQ_DELIVERY_STREAM`
- Builds a deterministic `O3DS::SubjectList` payload and pushes it through `moq_publish_data`

**Expected output:**
```
Publish.Connect: waiting for MOQ_STATE_CONNECTED
Namespace announcement succeeds
Publisher creation succeeds
Payload should not be empty: true
moq_publish_data returns OK
```

### Test: PublishSubscribe

**What it does:**
- Spins up two session harnesses (publisher + subscriber)
- Subscribes first, wiring `Config.OnData` to flip an atomic flag when bytes arrive
- Announces namespace + publisher
- Publishes a serialized payload and waits (latently) until the subscriber callback fires

**Expected output:**
```
PubSub.PublisherConnect: waiting for MOQ_STATE_CONNECTED
PubSub.SubscriberConnect: waiting for MOQ_STATE_CONNECTED
Subscriber creation succeeds
Publisher for pub/sub created
PubSub.Payload: payload received ✓
```

### Test: Sender

**What it does:**
- Wraps `FO3DMoQSender` in a harness that initializes/start/stops the sender
- Generates five synthetic `O3DS::SubjectList` frames and calls `Sender.Send()` for each
- Uses `FWaitForSenderFramesCommand` to tick the sender until at least two frames are reported as sent

**Expected output:**
```
Sender initialization should succeed
Sender start should succeed
Sender.Frames: expected 2 frames but only 1 sent (retry)
Sender.Frames: ✓ (frames sent >= 2)
```

### Test: MultiPublisher

**What it does:**
- Spawns three independent session harnesses
- Waits for each to reach `MOQ_STATE_CONNECTED`
- Announces namespaces that include the harness index and publishes a payload from each
- Validates that the number of publisher handles created matches the number of harnesses

**Expected output:**
```
Multi.Connect.0: waiting for MOQ_STATE_CONNECTED
Multi.Connect.1: waiting for MOQ_STATE_CONNECTED
Multi.Connect.2: waiting for MOQ_STATE_CONNECTED
Publisher 0 announce: success
Publisher 1 announce: success
Publisher 2 announce: success
Publisher handles == harness count: true
```

## Reliability Tips

1. **Prefer local relay for CI.** Set `O3D_MOQ_RELAY_URL=https://127.0.0.1:4443` (or similar) and run a local `moq-relay-ietf` instance so tests do not depend on the public Cloudflare relay.
2. **Budget extra time.** Latent commands use 10–20 second timeouts. When running the full group, expect ~120 seconds before the automation framework reports success.
3. **Inspect Output Log.** Each harness logs its label (e.g., `Publish.Connect`) when failures occur. Use these breadcrumbs to decide whether a relay outage or local configuration issue is to blame.
4. **Retry before triage.** Because tests hit a real network, rerunning the suite once often distinguishes transient relay hiccups from regressions.

## Troubleshooting

### "MoQ FFI library not loaded"

**Problem:** The moq_ffi.dll is not available or not loaded by the module.

**Solutions:**
1. Check that `ThirdParty/moq-ffi/bin/Win64/Release/moq_ffi.dll` exists **and matches the SHA256 listed in `ThirdParty/moq-ffi/README.md`**
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
4. Try increasing timeout in test code if needed, or point the tests at a local `moq-relay-ietf` instance to remove the internet variable

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

1. Integrate these tests into a repeatable automation job (ideally backed by a local relay) so regressions are caught before merges.
2. Capture sample logs and screenshots for each test to include in troubleshooting guides and PR templates.
3. Build on this coverage by implementing the Phase 3 receiver tests (Initialize/Start/Subscribe/Poll) and adding richer MoQ stats to `FO3DMoQSender::GetStats()`.

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
