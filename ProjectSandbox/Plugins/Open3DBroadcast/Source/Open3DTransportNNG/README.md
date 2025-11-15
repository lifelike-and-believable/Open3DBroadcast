# Open3D Transport NNG Module

High-performance transport for Open3D mocap data using NNG (nanomsg-next-gen).

## Protocols Supported

- **Pub/Sub**: Publisher/Subscriber (one-to-many broadcast)
- **Push/Pull**: Pipeline (one-to-one with built-in load balancing)
- **Pair**: Bidirectional (one-to-one)

## Configuration

### URI Format

```
nng://<host>:<port>?mode=<mode>&role=<role>
```

Where:
- `host`: IP address or hostname
- `port`: TCP port number
- `mode`: `pub`, `sub`, `push`, `pull`, `pair` (default: `pub`)
- `role`: `server`, `client` (default: `server` for pub, `client` for sub)

### Example Configurations

**Localhost Pub/Sub (default):**
```
nng://127.0.0.1:5555
```

**Cloud Push/Pull via Repeater:**
- Sender (Push): `tcp://52.89.47.134:7000`
- Repeater Listen: `tcp://0.0.0.0:7000` (receives Push from sender)
- Repeater Broadcast: `tcp://0.0.0.0:7001` (publishes to receivers)
- Receiver (Subscribe): `tcp://52.89.47.134:7001`

## Troubleshooting

### No Connection / Queue Full Warnings

If you see "NNG sender queue full" warnings and no animation on the receiver:

1. **Verify ports match**: Check that sender/receiver ports match the repeater configuration
   - Sender should connect to repeater's listen port (e.g., 7000)
   - Receivers should connect to repeater's broadcast port (e.g., 7001)

2. **Check network connectivity**: Ping the remote host and verify firewall rules allow the ports

3. **Enable verbose logging**: Monitor the logs for connection establishment messages
   - Look for "NNG sender pipe added" = connection successful
   - Look for "NNG sender pipe removed" = connection lost

### High Latency / Cloud Connections

The sender uses non-blocking sends with automatic retry to handle high-latency cloud connections gracefully:
- Frames are re-queued if the socket buffer is full, rather than being dropped
- The worker thread never blocks on network I/O
- A 30-second send timeout prevents indefinite hangs on dead connections

If you still see frame drops with high latency:
- Increase the `MaxQueueBytes` setting (default 4MB)
- Consider increasing the repeater's buffer size (`main.cpp` line ~47)

## Performance Notes

- Single subject mocap: ~4 KB per frame at 30 FPS = ~120 KB/sec
- Audio (PCM16, 48kHz stereo): ~4 KB per frame at 50 FPS = ~200 KB/sec
- Combined with Opus compression: ~240 bytes per audio frame

Even slow cloud connections (10+ Mbps) can easily handle this bandwidth.
