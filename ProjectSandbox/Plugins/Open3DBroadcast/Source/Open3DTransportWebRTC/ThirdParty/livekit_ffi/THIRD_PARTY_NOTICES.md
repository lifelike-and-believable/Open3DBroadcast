# Third-Party Notices — livekit_ffi.dll

`livekit_ffi.dll` is a statically linked Rust `cdylib` built with LTO. Everything
listed here is compiled **into** the DLL that ships with this plugin; none of it
is a runtime dependency that a user could substitute.

## How this list was produced

The crate versions below were recovered by reading the Cargo registry paths that
`rustc` embeds in the binary (`C:\Users\runneradmin\.cargo\registry\src\index.crates.io-*\<crate>-<version>\...`),
then each version's `license` field was read from the crates.io API. The
components in §2 were confirmed by locating their symbols and strings directly in
the shipped DLL.

⚠️ **This is a stopgap, not a build product.** It was reconstructed from binary
inspection after the fact, and it will drift silently the next time the DLL is
rebuilt. See "Maintaining this file" at the end.

---

## 1. Rust crates

| Crate | Version | License (SPDX) |
| --- | --- | --- |
| anyhow | 1.0.100 | MIT OR Apache-2.0 |
| atomic-waker | 1.1.2 | Apache-2.0 OR MIT |
| base64 | 0.21.7 | MIT OR Apache-2.0 |
| base64 | 0.22.1 | MIT OR Apache-2.0 |
| bmrng | 0.5.2 | MIT OR Apache-2.0 |
| bytes | 1.10.1 | MIT |
| chrono | 0.4.42 | MIT OR Apache-2.0 |
| cxx | 1.0.187 | MIT OR Apache-2.0 |
| data-encoding | 2.9.0 | MIT |
| form_urlencoded | 1.2.2 | MIT OR Apache-2.0 |
| futures-channel | 0.3.31 | MIT OR Apache-2.0 |
| futures-core | 0.3.31 | MIT OR Apache-2.0 |
| futures-util | 0.3.31 | MIT OR Apache-2.0 |
| http | 0.2.12 | MIT OR Apache-2.0 |
| http | 1.3.1 | MIT OR Apache-2.0 |
| http-body-util | 0.1.3 | MIT |
| httparse | 1.10.1 | MIT OR Apache-2.0 |
| hyper | 1.7.0 | MIT |
| hyper-rustls | 0.27.7 | Apache-2.0 OR ISC OR MIT |
| hyper-util | 0.1.17 | MIT |
| icu_normalizer | 2.1.1 | **Unicode-3.0** |
| idna | 1.1.0 | MIT OR Apache-2.0 |
| ipnet | 2.11.0 | MIT OR Apache-2.0 |
| iri-string | 0.7.9 | MIT OR Apache-2.0 |
| lazy_static | 1.5.0 | MIT OR Apache-2.0 |
| libwebrtc | 0.3.19 | Apache-2.0 |
| livekit | 0.7.24 | Apache-2.0 |
| livekit-api | 0.4.9 | Apache-2.0 |
| livekit-protocol | 0.5.1 | Apache-2.0 |
| livekit-runtime | 0.4.0 | Apache-2.0 |
| mio | 1.1.0 | MIT |
| once_cell | 1.21.3 | MIT OR Apache-2.0 |
| parking_lot | 0.12.5 | MIT OR Apache-2.0 |
| parking_lot_core | 0.9.12 | MIT OR Apache-2.0 |
| percent-encoding | 2.3.2 | MIT OR Apache-2.0 |
| prost | 0.12.6 | Apache-2.0 |
| rand | 0.8.5 | MIT OR Apache-2.0 |
| reqwest | 0.12.24 | MIT OR Apache-2.0 |
| ring | 0.17.14 | **Apache-2.0 AND ISC** (see note) |
| rustls | 0.21.12 | Apache-2.0 OR ISC OR MIT |
| rustls | 0.23.35 | Apache-2.0 OR ISC OR MIT |
| rustls-pki-types | 1.13.0 | MIT OR Apache-2.0 |
| rustls-webpki | 0.101.7 | ISC |
| rustls-webpki | 0.103.8 | ISC |
| sct | 0.7.1 | Apache-2.0 OR ISC OR MIT |
| serde_core | 1.0.228 | MIT OR Apache-2.0 |
| serde_json | 1.0.145 | MIT OR Apache-2.0 |
| smallvec | 1.15.1 | MIT OR Apache-2.0 |
| socket2 | 0.6.1 | MIT OR Apache-2.0 |
| tokio | 1.48.0 | MIT |
| tokio-rustls | 0.24.1 | MIT OR Apache-2.0 |
| tokio-rustls | 0.26.4 | MIT OR Apache-2.0 |
| tokio-tungstenite | 0.20.1 | MIT |
| tower | 0.5.2 | MIT |
| tungstenite | 0.20.1 | MIT OR Apache-2.0 |
| untrusted | 0.9.0 | ISC |
| url | 2.5.7 | MIT OR Apache-2.0 |
| utf-8 | 0.7.6 | MIT OR Apache-2.0 |
| want | 0.3.1 | MIT |
| webrtc-sys | 0.3.16 | Apache-2.0 |

### Note on `ring`

`ring` declares `Apache-2.0 AND ISC` — a **conjunctive** AND, not the usual
`OR`. Both sets of terms apply at once, and the crate's top-level `LICENSE` is a
pointer file rather than a grant: new code is ISC (`LICENSE-other-bits`), code
sourced from BoringSSL is Apache-2.0 (`LICENSE-BoringSSL`), the `once_cell`
polyfill is dual Apache-2.0/MIT, and `third_party/fiat` is Apache-2.0. Per-file
licensing is declared in file headers. All five files should be reproduced.

### Note on `icu_normalizer`

Unicode-3.0 is a distinct license (not MIT/Apache) and requires reproducing the
Unicode license and trademark notice.

---

## 2. Native components inside libwebrtc

`webrtc-sys` does not compile libwebrtc from source here — it downloads a
prebuilt static library. That library is linked into `livekit_ffi.dll`,
bringing **27 native components** with it.

The exact artifact is pinned in `webrtc-sys-build` 0.3.11 (the build dependency
of `webrtc-sys` 0.3.16), read from the published crate source:

| Item | Value |
| --- | --- |
| Pinned tag | `WEBRTC_TAG = "webrtc-ebd5a9f-2"` |
| Download URL | `https://github.com/livekit/client-sdk-rust/releases/download/webrtc-ebd5a9f-2/webrtc-<os>-<arch>-<profile>.zip` |
| Artifact for this DLL | `webrtc-windows-x64-release.zip` |

> **Correction.** An earlier revision of this file recorded the artifact as
> `webrtc-sdk/webrtc @ m137_release` under release tag `webrtc-7af9351`. That
> was inferred rather than read from the pin, and it is wrong — the version
> above is taken directly from `webrtc-sys-build` 0.3.11's source.

The upstream Windows x64 artifact ships a generated `LICENSE.md` enumerating
them: webrtc, abseil-cpp, boringssl, compiler-rt, crc32c, dav1d, **ffmpeg**,
fft, fiat, g711, g722, libaom, libc++, libjpeg_turbo, libsrtp, libvpx, libyuv,
nasm, ooura, **openh264**, opus, perfetto, pffft, protobuf, rnnoise,
spl_sqrt_floor, zlib. The mix is predominantly BSD-2-Clause, BSD-3-Clause,
Apache-2.0, Apache-2.0-with-LLVM-exception (libc++, compiler-rt) and Zlib.

libwebrtc itself is **BSD-3-Clause** and ships a separate `PATENTS` file — an
Additional IP Rights Grant from Google with a patent-litigation termination
clause. That grant is *not* part of BSD-3-Clause and must be reproduced
alongside it.

> **That upstream `LICENSE.md` is the authoritative per-build notice and is not
> currently vendored into this repository.** It should be taken from the
> matching artifact (`webrtc-ebd5a9f-2`, above) and committed next to this file
> before distribution. It is also platform-specific — the Linux/macOS builds use
> different `gn` args and therefore a different component list, so each shipped
> platform needs its own copy.

### ⚠️ ffmpeg and OpenH264 — unresolved, needs legal review

Presence of both was **verified directly in the DLL that ships in this plugin**,
not merely inferred from the upstream component list:

| Component | Evidence in `livekit_ffi.dll` |
| --- | --- |
| ffmpeg | `libavcodec`, `libavutil`, `avcodec_send_packet`, `avcodec_receive_frame`, `avcodec_open`, `ffmpeg.org`, `"FFmpeg H.264 decoder not found."` |
| OpenH264 | `OpenH264`, `openh264`, `WelsEnc` |

Why this matters:

- **ffmpeg is copyleft.** The upstream notice file reproduces GPLv2, GPLv3,
  LGPLv2.1 *and* LGPLv3 in its ffmpeg section, because the generator emits every
  `COPYING.*` file in the tree — so it does not by itself establish which
  license is operative. The build is understood to use Chromium's
  `ffmpeg_branding = "Chrome"` configuration, which excludes GPL-only components
  and should leave an LGPL-2.1-or-later build. **This has not been confirmed
  against the actual artifact and must not be assumed.**
- Because ffmpeg is **statically** linked, LGPL §6 relinking obligations
  plausibly attach to redistributing the DLL — that is a materially different
  obligation from the permissive licenses covering everything else here, and it
  is not discharged by attribution alone.
- **OpenH264** is BSD-2-Clause (Cisco Systems, 2013) for copyright purposes, but
  Cisco's royalty-free H.264 patent offer covers *their own prebuilt binary
  module*, not a statically recompiled OpenH264. H.264 patent licensing (Via LA
  / Access Advance) is a separate question from copyright attribution and is not
  resolved by this file.

**This is a release blocker for commercial distribution and cannot be closed by
documentation.** It needs either counsel sign-off on the LGPL and patent
posture, or a libwebrtc build with the ffmpeg/H.264 components disabled.

### Neither component is reachable from this plugin

The `livekit_ffi` C ABI exposes **no video path at all** — "video" does not
appear anywhere in `include/livekit_ffi.h`, and no source file in
`Open3DTransportWebRTC` references video. This transport carries mocap over the
data channel plus Opus audio. ffmpeg and OpenH264 arrive purely because the
prebuilt libwebrtc was built with H.264 enabled.

That does **not** reduce the obligation — LGPL attaches to distributing the
bytes, not to executing them — but it does mean removing them costs zero
functionality.

**The chosen fix is to rebuild libwebrtc with H.264 disabled**, keeping the
WebRTC transport intact. The mechanism, the one known risk, and step-by-step
acceptance criteria are in
[`docs/webrtc-codec-removal-plan.md`](../../../../../../../docs/webrtc-codec-removal-plan.md).

---

## Maintaining this file

Do not hand-edit this after the next DLL rebuild. Add a `cargo about generate`
(or `cargo deny`) step to the `livekit-ffi-ue` CI workflow, publish its output as
a release asset alongside the DLL, and copy it in as part of the refresh
workflow in `README.md`. Vendor the upstream libwebrtc `LICENSE.md` at the same
time.
