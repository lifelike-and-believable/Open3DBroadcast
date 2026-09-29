## Unreleased

### Fixed

- UDP fragment reassembly (`src/o3ds/udp_fragment.*`, WP-S2) is hardened against hostile or malformed datagrams: fragments that disagree with a message's first fragment are rejected (previously a heap overflow), in-flight state is bounded (8 messages, 16 MiB by default) with age-based expiry, message ids use wrapping comparison, messages are keyed per sender, rejected fragments no longer yield empty frames, and a use-after-free in `UdpMapper::getFrame` is gone.
- UE UDP receiver: `Poll()` is bounded per call (1024 datagrams / 8 MiB), the receive buffer is always 65,507 bytes regardless of `udp.maxdatagram`, and receive scratch buffers are reused.

### Changed

- The largest reassembled UDP message the receiver accepts drops from 50 MiB to 4 MiB by default; set the new `udp.maxframe` receiver option to raise it (up to 50 MiB).
- Core API: `UdpMapper::addFragment` now takes `(sourceKey, data, size, nowMs)`, and `UdpMapper` takes an optional `UdpReassemblyConfig`.

### Schema/Protocol

- No wire change. The 16-byte UDP fragment header is now read and written as explicit little-endian, which is byte-for-byte identical to what existing little-endian senders and receivers produce. The versioned `O3DF` header (ADR 0009, TRB-17) follows separately.