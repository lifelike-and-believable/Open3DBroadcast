# Third-Party Licenses: Open3DBroadcastWebRTC

The Open3DBroadcastWebRTC add-on plugin (the WebRTC transport for
Open3DBroadcast) redistributes one prebuilt third-party binary,
`livekit_ffi.dll`. This file is the inventory of what ships inside this plugin
folder and the license obligations that attach to redistributing it.
Open3DBroadcast itself has its own `THIRD_PARTY_LICENSES.md`, which covers the
code this add-on links against (the Open3DStream core, FlatBuffers, CRC++,
Opus).

This add-on was split out of Open3DBroadcast so that the Fab package of
Open3DBroadcast contains none of the code below (ADR 0002, WP-F11). Whether and
when this add-on can be published before the codec-free rebuild is counsel
question L1 in that ADR.

**One exception:** `livekit_ffi` has **no shipped license text**, because none
exists upstream to copy — the wrapper declares MIT but publishes no `LICENSE`
file. This was verified directly against the upstream repository, not inferred.
Its row links to the per-binary notices instead. That gap is an open obligation,
not an oversight in this index; see the note below.

---

## ⚠️ Unresolved blocker: copyleft code in `livekit_ffi.dll`

**`livekit_ffi.dll` statically links ffmpeg and OpenH264.** This was verified by
symbol inspection of the exact DLL committed to this repository — `libavcodec`,
`libavutil`, `avcodec_send_packet`, `avcodec_receive_frame`, `ffmpeg.org`, and
`OpenH264` / `WelsEnc` are all present in the binary. They arrive transitively
through `webrtc-sys`, which links a prebuilt libwebrtc containing 27 native
components.

ffmpeg is copyleft. Even under the LGPL-2.1 configuration this build is believed
to use, static linking plausibly triggers LGPL §6 relinking obligations — an
obligation that attribution alone does not discharge. OpenH264 additionally
raises H.264 patent-licensing questions distinct from copyright.

**This cannot be closed by documentation.** It needs either counsel sign-off on
the LGPL and patent posture, or a libwebrtc build with ffmpeg/H.264 disabled.
Full evidence and reasoning:
[`Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/THIRD_PARTY_NOTICES.md`](Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/THIRD_PARTY_NOTICES.md).

**Neither component is reachable from this add-on.** The `livekit_ffi` C ABI has
no video path whatsoever — this transport carries mocap over the data channel
plus Opus audio. They arrive only because the prebuilt libwebrtc was built with
H.264 enabled. That doesn't reduce the obligation (LGPL attaches to
distribution, not execution), but it does mean removing them costs no
functionality.

**Chosen fix: rebuild libwebrtc with H.264 disabled**, keeping the WebRTC
transport. Plan, mechanism, and acceptance criteria:
[`docs/webrtc-codec-removal-plan.md`](../../../docs/webrtc-codec-removal-plan.md).

`moq_ffi.dll` in Open3DBroadcast was checked for the same components and is clean.

---

## 1. The plugin itself

| Component | License | Text |
| --- | --- | --- |
| Open3DBroadcastWebRTC (`Source/Open3DTransportWebRTC`) | MIT | [`LICENSE`](LICENSE) |

---

## 2. Prebuilt binaries vendored in the plugin

| Component | Version | License | Text | Consumed by |
| --- | --- | --- | --- | --- |
| livekit_ffi | see note | MIT (**declared, but no upstream text exists** — see note) | [`THIRD_PARTY_NOTICES.md`](Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/THIRD_PARTY_NOTICES.md) | `Open3DTransportWebRTC` |

### livekit_ffi

`livekit_ffi.dll` is a statically linked Rust binary built from
[`lifelike-and-believable/livekit-ffi`](https://github.com/lifelike-and-believable/livekit-ffi).
Full provenance, artifact hashes, and the recovered dependency graph are in
[`Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/README.md`](Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/README.md),
with the per-crate license inventory in
[`THIRD_PARTY_NOTICES.md`](Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/THIRD_PARTY_NOTICES.md)
next to it.

Two obligations attach that are **not** satisfied by the MIT declaration alone:

1. **The upstream MIT grant has no license text.** Verified against the upstream
   repository at `abfcd7b`: `livekit-ffi` declares `license = "MIT"` in
   `Cargo.toml`, and its root contains **no `LICENSE` and no `COPYING`** — no
   copyright holder, no year, no notice. MIT requires the notice to accompany
   redistributions, and this plugin redistributes the DLL, so this must be fixed
   upstream and the file copied into the `livekit_ffi/` directory before
   submission.
2. **The DLL is a combined work.** `livekit`, `livekit-api`, and
   `livekit-protocol` are Apache-2.0 and are statically linked into it with LTO,
   so Apache-2.0 §4 attribution and NOTICE obligations apply to the DLL itself.
   Google's libwebrtc (BSD-3-Clause, plus its own bundled third-party code) is
   also linked in via `webrtc-sys`.

---

## 3. Plugin dependencies

`Open3DBroadcastWebRTC.uplugin` declares one plugin dependency, Open3DBroadcast,
which is installed separately (from Fab or from a GitHub release). No
Open3DBroadcast code is redistributed in this plugin.
