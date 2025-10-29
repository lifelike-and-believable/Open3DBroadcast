# Open3DStream WebRTC Docs Index

This area collects the documents for the WebRTC refactor (Issue #94), backend selection, and audio integration.

## Core Documentation

- [WEBRTC_BACKENDS.md](./WEBRTC_BACKENDS.md) — Backends (P2P libdatachannel and LiveKit), selection, config, build flags
- [WEBRTC_AUDIO_SUBJECT_ASSOCIATION.md](./WEBRTC_AUDIO_SUBJECT_ASSOCIATION.md) — Subject-aware audio track labeling and announce message
- [WEBRTC_CONNECTOR_INTERFACE.md](./WEBRTC_CONNECTOR_INTERFACE.md) — Backend-agnostic connector interface and factory
- [UE_AUDIO_COMPONENTS.md](./UE_AUDIO_COMPONENTS.md) — UE components for capture/playback and subject routing
- [LIVEKIT_DATA_MESSAGING.md](./LIVEKIT_DATA_MESSAGING.md) — Mapping DataChannel semantics to LiveKit reliable/lossy data
- [BUILD_LIBDATACHANNEL_MEDIA.md](./BUILD_LIBDATACHANNEL_MEDIA.md) — Build libdatachannel with media/Opus for P2P audio
- [WEBRTC_AUDIO_STATUS_2025-10-27.md](./WEBRTC_AUDIO_STATUS_2025-10-27.md) — Current implementation status of audio features

## Planning & Roadmap

- [WEBRTC_AUDIO_PLANNING_SUMMARY.md](../WEBRTC_AUDIO_PLANNING_SUMMARY.md) — **START HERE**: Executive summary, timeline, next steps
- [WEBRTC_AUDIO_INTEGRATION_PLAN.md](../WEBRTC_AUDIO_INTEGRATION_PLAN.md) — Comprehensive plan for full audio integration (core library, multi-track, LiveKit, scale-out)
- [WEBRTC_AUDIO_ROADMAP.md](../WEBRTC_AUDIO_ROADMAP.md) — Quick reference implementation roadmap with checklists and examples