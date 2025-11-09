# Issue #11 — Obsolete/Superseded (October 2025)

> This document is obsolete and retained only to preserve incoming links. The project did not migrate to Unreal’s Pixel Streaming for WebRTC data channels. Instead, Open3DStream uses libdatachannel with a lightweight WebSocket signaling server. Do not re-introduce the Pixel Streaming stubs described in earlier revisions of this file.

## Where to look instead

- WebRTC quick start for C++ tools: `WEBRTC_QUICKSTART.md`
- Unreal plugin WebRTC notes (libdatachannel path): `WEBRTC_UNREAL_IMPLEMENTATION.md`
- Full WebRTC support guide: `WEBRTC_SUPPORT.md`
- libdatachannel integration details: `LIBDATACHANNEL_INTEGRATION.md`

## Summary of the decision

- libdatachannel is integrated and enabled-by-default in the core library.
- The Unreal plugin implements a libdatachannel-based WebRTC connector (functional, Beta) using a simple WebSocket signaling server.
- Pixel Streaming–based data channel work was explored but is not the chosen path for this project.

## Please don’t do this

- Don’t remove libdatachannel or its build/linkage from the Unreal plugin.
- Don’t restore Pixel Streaming stubs/classes as the primary WebRTC transport.
- Don’t change docs back to “WebRTC pending” for Unreal; it’s functional (Beta).

## Historical reference

The pre-refresh content of this file described a Pixel Streaming migration pathway. It has been removed to prevent regressions. If you need the historical discussion, refer to the Git history of this file prior to 2025-10-17.
