# Removing ffmpeg and OpenH264 from `livekit_ffi.dll`

**Goal:** eliminate the copyleft/patent blocker without losing the WebRTC
transport.

**Status:** plan only — no build has been attempted. Everything in §1–§3 was
read from published crate source and from the shipped binary; §4 is the part
that needs a machine to prove out.

---

## 0. Why this is worth doing

`livekit_ffi.dll` links ffmpeg and OpenH264, and neither is reachable from this
plugin. The `livekit_ffi` C ABI exposes **no video path at all** — the word
"video" does not appear in `include/livekit_ffi.h`, and no source file in
`Open3DTransportWebRTC` references video. The transport is mocap-over-data-channel
plus Opus audio.

So the plugin ships an entire video codec stack that no call path can reach:

| Component | In the DLL | Reachable from our API | License posture |
| --- | --- | --- | --- |
| ffmpeg (`libavcodec`, `libavutil`) | yes | **no** | **LGPL-2.1 (copyleft)** |
| OpenH264 (`WelsEnc`) | yes | **no** | BSD-2 + **H.264 patents** |
| libvpx (VP8/VP9) | yes | no | BSD-3 (fine) |
| dav1d / libaom (AV1) | yes | no | BSD-2 / BSD-3 (fine) |

Note `avformat`, `swscale` and `swresample` are **absent** — this is the
decoder-only libavcodec subset, which is the signature of Chromium's ffmpeg
configuration and supports reading it as LGPL-2.1 rather than GPL.

> ⚠️ **"We don't call it" is not a defense.** LGPL obligations attach to
> *distributing* the bytes, not to whether they execute. Unreachability doesn't
> shrink the obligation — it just means removing the code costs us **zero
> functionality**.

---

## 1. How `webrtc-sys` gets libwebrtc

Read from the published sources of `webrtc-sys` 0.3.16 and its build dependency
`webrtc-sys-build` 0.3.11 (both pulled from crates.io).

`webrtc-sys` does **not** build libwebrtc. `webrtc_sys_build::webrtc_dir()`
resolves to:

1. `$LK_CUSTOM_WEBRTC`, if that environment variable is set — *"the location of
   the custom build is defined by the user"*; or
2. a scratch directory holding a prebuilt zip downloaded from
   `https://github.com/livekit/client-sdk-rust/releases/download/webrtc-ebd5a9f-2/webrtc-<os>-<arch>-<profile>.zip`

The artifact must provide `include/`, `lib/`, and a `webrtc.ninja`.

## 2. How codec configuration propagates — the key mechanism

`webrtc-sys`'s `build.rs` does:

```rust
for (key, value) in webrtc_sys_build::webrtc_defines() {
    builder.define(key.as_str(), value);
}
```

and `webrtc_defines()` **reads the first line of `webrtc.ninja` from the
artifact** and scrapes every `-DFOO[=BAR]` out of it.

That means the preprocessor defines used to compile `webrtc-sys`'s own C++ shim
are inherited from however libwebrtc itself was built. **Turning H.264 off in
the libwebrtc `gn` args automatically turns it off in `webrtc-sys`** — no fork
of `webrtc-sys` is needed for the wiring.

This is confirmed empirically in reverse: `WelsEnc` symbols are present in our
shipped DLL, which is only possible if `WEBRTC_USE_H264` was defined, which is
only possible if the prebuilt artifact was built with H.264 enabled.

## 3. What's already conditional, and what isn't

**Encoder — fully guarded.** `src/video_encoder_factory.cpp`:

```cpp
using Factory = webrtc::VideoEncoderFactoryTemplate<
    webrtc::LibvpxVp8EncoderTemplateAdapter,
#if defined(WEBRTC_USE_H264)
    webrtc::OpenH264EncoderTemplateAdapter,
#endif
#if defined(RTC_USE_LIBAOM_AV1_ENCODER)
    webrtc::LibaomAv1EncoderTemplateAdapter,
#endif
    webrtc::LibvpxVp9EncoderTemplateAdapter>;
```

Drop `WEBRTC_USE_H264` and the OpenH264 encoder disappears cleanly.

**Decoder — NOT guarded.** `src/video_decoder_factory.cpp` calls, with no
`#if` around either:

- line 75 — `webrtc::SupportedH264DecoderCodecs()`
- line 114 — `webrtc::H264Decoder::Create()`

This asymmetry (encoder guarded, decoder not) looks like an upstream oversight,
and it is the single biggest unknown in this plan. See §4.

**Leave VP8/VP9 alone.** They're unconditional in the encoder template, and
libvpx is BSD-3-Clause — permissive, not part of the problem. Removing them
would require patching `webrtc-sys`; keeping them means we don't have to.
Same reasoning for AV1.

**So the minimal correct change targets H.264 only** — which is also what
removes ffmpeg, since libwebrtc's ffmpeg dependency exists to service the H.264
decoder (`H264DecoderImpl`). One flag should drop both.

---

## 4. Plan

### Step 1 — build libwebrtc with H.264 disabled

Build the LiveKit libwebrtc fork for `windows-x64-release` with H.264 off. The
flag to start from is:

```
rtc_use_h264 = false
```

and defensively, so no proprietary codec set can be selected:

```
ffmpeg_branding = "Chromium"
```

> **Verify these names against the branch you build.** `gn` arg names drift
> between WebRTC milestones, and `rtc_use_h264` in particular has changed
> meaning/spelling historically. Confirm against that tree's `.gn`/`BUILD.gn`
> rather than trusting this document.

**Acceptance for this step:** the resulting `webrtc.ninja` first line does *not*
contain `-DWEBRTC_USE_H264`, and the built archive contains no `libavcodec`,
`avcodec_send_packet`, `OpenH264`, or `WelsEnc` strings.

### Step 2 — resolve the unguarded decoder references

Build `webrtc-sys` against the Step 1 output with:

```
LK_CUSTOM_WEBRTC=/path/to/libwebrtc-no-h264 cargo build --release --features with_livekit
```

Two possible outcomes:

- **It compiles.** Then libwebrtc still provides stub implementations of
  `SupportedH264DecoderCodecs()` / `H264Decoder::Create()` when H.264 is
  disabled, and nothing further is needed.
- **It fails to compile or link** on those two symbols. Then apply a small patch
  guarding both call sites exactly as the encoder ones already are:

  ```cpp
  #if defined(WEBRTC_USE_H264)
    for (const webrtc::SdpVideoFormat& h264_format :
         webrtc::SupportedH264DecoderCodecs())
      formats.push_back(h264_format);
  #endif
  ```

  This is a good upstream PR to LiveKit — it fixes a real inconsistency and
  removes the need to carry a fork.

**Do not skip to patching.** Determine which case applies first; the no-patch
path is much cheaper to maintain.

### Step 3 — wire it into `livekit-ffi-ue` CI

Set `LK_CUSTOM_WEBRTC` in `.github/workflows/build-ffi.yml`, pointing at either
a cached custom libwebrtc build or one restored from a release asset. Building
libwebrtc from scratch on every run is not viable — it's an hours-long,
tens-of-GB build — so publish it once per libwebrtc bump and cache it.

### Step 4 — verify the shipped artifact

Before accepting the new DLL, re-run the scan that found the problem:

```sh
for pat in avcodec avutil openh264 WelsEnc; do
  echo "$pat: $(strings -a livekit_ffi.dll | grep -ic "$pat")"
done
```

**All four must be 0.** Also confirm the transport still works end to end —
mocap over the data channel and Opus audio both flowing — since that is the
functionality we're protecting.

### Step 5 — close out the compliance docs

- Update `THIRD_PARTY_NOTICES.md` §2: drop the ffmpeg/OpenH264 blocker section,
  record the new artifact's tag and hashes.
- Update the plugin-root `THIRD_PARTY_LICENSES.md`, which currently opens with
  the blocker.
- Vendor the new build's libwebrtc `LICENSE.md` (still outstanding regardless).
- Add `cargo about generate` to the `livekit-ffi-ue` workflow so notices stop
  being hand-maintained.

---

## 5. Expected outcome

- **ffmpeg and OpenH264 gone** — copyleft and H.264 patent questions both close,
  with no counsel sign-off needed.
- **WebRTC transport unchanged** — nothing removed was reachable from its API.
- **Smaller DLL.** The current one is 23 MB; a meaningful share is video codec
  code. Not quantified — measure after Step 1.

## 6. What could go wrong

| Risk | Mitigation |
| --- | --- |
| Unguarded decoder refs break the build (§3) | Step 2 decides; small patch is the fallback |
| `gn` arg names differ on the target branch | Verify against that tree before building |
| libwebrtc build is heavy to stand up and own | Build once per bump, cache the artifact, don't rebuild per CI run |
| Audio path regresses unnoticed | Step 4 requires an end-to-end functional check, not just a symbol scan |
| A future LiveKit SDK bump silently re-enables H.264 | Make the Step 4 symbol scan a CI assertion, not a manual step |

That last one matters most: without an automated check, this regresses the
first time someone bumps the SDK and forgets.
