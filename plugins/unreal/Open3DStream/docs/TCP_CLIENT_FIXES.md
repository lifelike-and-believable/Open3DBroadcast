# TCP Client Connection Fixes

## Overview
Fixed several critical issues in the TCP client implementation in `UOpen3DServer.cpp` that were causing connection instability, failed reconnection attempts, and misleading status messages.

## Issues Fixed

### 1. Socket Recreation During Pending Connection
**Problem**: The original code would recreate the socket even when a connection was still pending, interrupting in-progress connection attempts.

**Fix**: Modified the reconnection logic to only create a new socket when `ConnState == ESocketConnectionState::SCS_NotConnected` AND `!mTcp`. This prevents recreating the socket while a connection attempt is in progress.

```cpp
// Before: Would recreate socket even during pending state
if (!mTcp || ConnState == ESocketConnectionState::SCS_NotConnected)

// After: Only recreate when truly disconnected
if (!mTcp && ConnState == ESocketConnectionState::SCS_NotConnected)
```

### 2. Incorrect Error Handling for ENOTCONN
**Problem**: The `ReadTcp()` function treated `SE_ENOTCONN` (not connected) as a non-error, returning false without tearing down the socket. This masked connection failures.

**Fix**: Removed `SE_ENOTCONN` from the "acceptable errors" list. Now only `SE_EWOULDBLOCK` and `SE_NO_ERROR` are treated as non-fatal (indicating no data available yet).

```cpp
// Before: Treated ENOTCONN as acceptable
if (Err == ESocketErrors::SE_EWOULDBLOCK || Err == ESocketErrors::SE_NO_ERROR || Err == ESocketErrors::SE_ENOTCONN)

// After: Only EWOULDBLOCK and NO_ERROR are acceptable
if (Err == ESocketErrors::SE_EWOULDBLOCK || Err == ESocketErrors::SE_NO_ERROR)
```

### 3. Missing Parser State Reset on Connection Error
**Problem**: When a connection error occurred in `ReadTcp()`, the parser state (`mState` and `mPtr`) wasn't reset, potentially causing protocol parsing errors on reconnection.

**Fix**: Added parser state reset when tearing down the socket due to errors:

```cpp
mState = eState::SYNC; // reset parser state on error
mPtr = 0;
```

### 4. Parser State Not Reset on Connection Error Detection
**Problem**: When `tick()` detected `SCS_ConnectionError`, it tore down the socket but didn't reset the parser state.

**Fix**: Added parser state reset in the connection error handling block:

```cpp
else if (ConnState == ESocketConnectionState::SCS_ConnectionError)
{
    // ...existing cleanup...
    mState = eState::SYNC; // reset parser state
    mPtr = 0;
}
```

### 5. Incorrect Initialization of mGoodTime
**Problem**: `mGoodTime` was initialized to `0.0f`, causing immediate "No Data" warnings even before any connection attempt.

**Fix**: Initialize `mGoodTime` to current time in constructor and reset it when starting TCP client:

```cpp
// In constructor
mGoodTime = FPlatformTime::Seconds();

// In start() for TCP Client
mGoodTime = FPlatformTime::Seconds(); // Reset timer to avoid immediate warning
```

### 6. "No Data" Warning Triggered Before Connection
**Problem**: The "No Data" warning would trigger even when not connected or during connection attempts.

**Fix**: Only check for "No Data" when there's an active connection:

```cpp
bool bHasActiveConnection = (mServer != nullptr) || (mWebRTCConnector != nullptr) || (mUdp != nullptr);
if (mTcp)
{
    ConnState = mTcp->GetConnectionState();
    bHasActiveConnection = bHasActiveConnection || (ConnState == ESocketConnectionState::SCS_Connected);
}

if (bHasActiveConnection && !mNoDataFlag && Now - mGoodTime > 1.0)
{
    OnState.ExecuteIfBound(LOCTEXT("NoData", "No Data"), true);
    mNoDataFlag = true;
}
```

### 7. Missing Initial Connection Status
**Problem**: No user feedback during initial connection attempts, making it unclear if the system was working.

**Fix**: Added status messages for initial connection and retries:

```cpp
// On initial connection
OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPConnecting", "TCP Connecting to {0}:{1}..."), 
    FText::FromString(mTcpIp), FText::AsNumber(mTcpPort)), false);

// On retry attempts (every 5th attempt)
if (mTcpBackoffAttempt % 5 == 1)
{
    OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPRetrying", "TCP Reconnecting to {0}:{1}... (attempt {2})"), 
        FText::FromString(mTcpIp), FText::AsNumber(mTcpPort), FText::AsNumber(mTcpBackoffAttempt)), false);
}
```

### 8. Parser State Could Get Stuck After Invalid Sync
**Problem**: When detecting invalid sync bytes, the parser didn't reset `mPtr`, potentially causing it to skip over valid sync sequences.

**Fix**: Reset `mPtr` when sync fails:

```cpp
if (!ok) 
{
    // Reset to start sync search over
    mPtr = 0;
    continue;
}
```

### 9. mGoodTime Type Inconsistency
**Problem**: `mGoodTime` was declared as `float` in header but used with `double` timing functions.

**Fix**: Changed declaration to `double` in header file for consistency:

```cpp
// In UOpen3DServer.h
double  mGoodTime;  // Changed from float
```

## Testing Recommendations

To validate these fixes:

1. **Connection Test**: Start TCP client before server is running
   - ? Should see "TCP Connecting to..." message
   - ? Should NOT see "No Data" warning immediately
   - ? Should see periodic retry messages
   - ? Should connect successfully when server starts

2. **Disconnection Test**: Stop server while client is connected
   - ? Should see "TCP Disconnected, retrying..." message
   - ? Should detect disconnection and tear down cleanly
   - ? Should automatically reconnect when server restarts

3. **Data Integrity Test**: Monitor for protocol parsing errors
   - ? Parser state should reset cleanly on reconnection
   - ? No "Malformed Data" errors after reconnect
   - ? Invalid sync bytes should be skipped properly

4. **Rapid Connect/Disconnect**: Start/stop server rapidly
   - ? Should handle connection state transitions gracefully
   - ? No socket leaks or crashes
   - ? Status messages should be clear and accurate

5. **Long-Running Stability**: Leave client running for extended period
   - ? Should reconnect automatically after server restart
   - ? No memory leaks in buffer management
   - ? Timing should remain accurate over time

## Related Files

- `Plugins\Open3DStream\Source\Open3DStream\Private\UOpen3DServer.cpp` - Fixed implementation
- `Plugins\Open3DStream\Source\Open3DStream\Public\UOpen3DServer.h` - Updated header with type fix
- `Plugins\Open3DStream\Source\Open3DBroadcast\Private\Transports\O3DSTcpTransport.cpp` - Reference implementation for TCP sender

## Impact

These fixes significantly improve the reliability of TCP client connections in the Open3DStream LiveLink source:

- **Stability**: Eliminates socket thrashing during connection attempts
- **Reliability**: Properly handles connection errors and reconnection
- **Data Integrity**: Ensures protocol parser state is consistent across reconnections
- **User Experience**: Clear status messages for connection state
- **Robustness**: Handles edge cases like rapid disconnect/reconnect cycles
- **Timing Accuracy**: Proper initialization and tracking of timing values

## Key Behavioral Changes

### Before Fixes:
- ? Immediate "No Data" warnings on startup
- ? No feedback during connection attempts
- ? Failed to reconnect reliably after server restart
- ? Parser could get stuck after connection errors
- ? Socket recreation interrupted pending connections

### After Fixes:
- ? Clean startup with appropriate status messages
- ? Clear feedback: "Connecting...", "Connected", "Reconnecting..."
- ? Reliable automatic reconnection with exponential backoff
- ? Parser properly resets and resynchronizes on reconnection
- ? Non-blocking connection attempts don't interfere with each other

## References

- Issue #70: M3.2 - TCP and UDP Broadcast Transports (Unreal)
- Transport Guide: `Plugins\Open3DStream\docs\BROADCAST_TRANSPORT_GUIDE.md`
- Planning: `Plugins\Open3DStream\docs\Open3DBroadcast_Planning.md`
