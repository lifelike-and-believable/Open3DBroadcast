# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- New Unreal shared module: `Open3DShared` providing:
  - WebRTC abstractions and implementations (connector, data channel, signaling)
  - Shared loopback registry for serialized frames (decouples Broadcast from Stream)
  - Logging categories and console variables
  - Helper utilities (subject name sanitize, wildcard matching, URL parsing/normalization)
  - Consolidated third-party linking (libdatachannel, OpenSSL, Opus)

### Changed
- `Open3DBroadcast` no longer depends on `Open3DStream`; audio capture component moved into Broadcast.
- `Open3DStream` registers a LiveLink-backed consumer via the shared loopback registry and now depends on `Open3DShared`.
- Built-in transports (e.g., NNG) use shared URL helpers; common tcp URL typos like `tcp://0.0.0.0.9000` are auto-normalized to `tcp://0.0.0.0:9000`.

### Compatibility
- No protocol/schema changes.
- Module boundaries changed in the Unreal plugin; consumer projects including headers from these modules should include from `Open3DShared` where applicable.

### Notes
- Consider future UI change to split Broadcast endpoint into separate Address/Port fields to avoid formatting mistakes; current URL field auto-corrects common tcp host/port typos.
