# Third-Party Licenses

The Open3D Broadcast Suite plugin redistributes third-party code, both as source
and as prebuilt binaries. This file is the consolidated inventory of everything
that ships inside the plugin folder and the license obligations that attach to
redistributing it.

Each entry links to the license text as it is shipped in this tree. Where a
dependency has its own directory, the license text lives next to the binary it
covers; that per-directory copy is authoritative, and this file is the index.

**One exception:** `livekit_ffi` has **no shipped license text**, because none
exists upstream to copy — the wrapper declares MIT but publishes no `LICENSE`
file. Its row links to the per-binary notices instead. That gap is an open
obligation, not an oversight in this index; see the note in §3.

**Scope:** this covers artifacts shipped *inside the plugin*. Build-time-only
dependencies that are not linked into any shipped binary are listed separately
at the end, along with the evidence for that claim.

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

`moq_ffi.dll` was checked for the same components and is clean.

---

## 1. The plugin itself

| Component | License | Text |
| --- | --- | --- |
| Open3D Broadcast Suite (all `Source/` modules) | MIT | [`LICENSE`](LICENSE) |
| Open3DStream core (`o3ds`) | MIT | [`ThirdParty/open3dstream/LICENSE`](ThirdParty/open3dstream/LICENSE) |

Both are MIT, © 2020–2024 Alastair Macleod.

`ThirdParty/open3dstream/` is **not** a vendored binary drop — its headers and
`open3dstreamstatic.lib` are compiled from `src/o3ds/` in this repository by
`Build/Scripts/Sync-O3DSCore.ps1` as part of the plugin build, and are
gitignored rather than committed.

---

## 2. Statically linked into `open3dstreamstatic.lib`

These are compiled into the o3ds core static library, so their code is present
in the shipped plugin binaries even though they have no directory of their own.
License texts are collected in
[`ThirdParty/open3dstream/THIRD_PARTY_LICENSES/`](ThirdParty/open3dstream/THIRD_PARTY_LICENSES).

| Component | Version | License | Text |
| --- | --- | --- | --- |
| Google FlatBuffers | 2.0.6 | Apache-2.0 | [`FlatBuffers-LICENSE.txt`](ThirdParty/open3dstream/THIRD_PARTY_LICENSES/FlatBuffers-LICENSE.txt) |
| CML (Configurable Math Library) | submodule `b2850e4` | BSL-1.0 | [`CML-LICENSE.txt`](ThirdParty/open3dstream/THIRD_PARTY_LICENSES/CML-LICENSE.txt) |
| CRC++ | submodule `71f2152` | BSD-3-Clause | [`CRCpp-LICENSE.txt`](ThirdParty/open3dstream/THIRD_PARTY_LICENSES/CRCpp-LICENSE.txt) |

CML is header-only and CRC++ is header-only; both are compiled in rather than
linked as separate libraries.

---

## 3. Prebuilt binaries vendored in the plugin

| Component | Version | License | Text | Consumed by |
| --- | --- | --- | --- | --- |
| Google FlatBuffers | 2.0.6 | Apache-2.0 | [`ThirdParty/flatbuffers/LICENSE.txt`](ThirdParty/flatbuffers/LICENSE.txt) | `Open3DShared`, `Open3DSender`, `Open3DReceiver` |
| Opus | see note | BSD-3-Clause + royalty-free patent grants | [`ThirdParty/opus/COPYING`](ThirdParty/opus/COPYING) | `Open3DShared` |
| NNG (nanomsg-next-gen) | 1.3.0 | MIT | [`Source/Open3DTransportNNG/ThirdParty/nng/LICENSE.txt`](Source/Open3DTransportNNG/ThirdParty/nng/LICENSE.txt) | `Open3DTransportNNG` |
| moq-ffi | commit `567933e` | MIT | [`Source/Open3DTransportMoQ/ThirdParty/moq-ffi/LICENSE`](Source/Open3DTransportMoQ/ThirdParty/moq-ffi/LICENSE) | `Open3DTransportMoQ` |
| livekit_ffi | see note | MIT (**declared, but no upstream text exists** — see note) | [`THIRD_PARTY_NOTICES.md`](Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/THIRD_PARTY_NOTICES.md) | `Open3DTransportWebRTC` |

### Opus

The shipped `opus.lib` reports its version string as `libopus unknown`, meaning
it was built from a git checkout with no version metadata. The API surface
present in the vendored headers (`OPUS_SET_PHASE_INVERSION_DISABLED`) puts it at
**Opus 1.2 or later**, but the exact version and build provenance are not
recorded anywhere in this repository. The `COPYING` text shipped here is the
canonical upstream one from [xiph/opus](https://github.com/xiph/opus).

Note that Opus carries **royalty-free patent licenses** (Xiph.Org, Microsoft,
Broadcom) whose terms are part of `COPYING`; these travel with binary
redistribution and are not optional.

> **Open item:** Unreal Engine already ships its own libopus. Whether this
> vendored copy is needed at all, or whether `Open3DShared` should link the
> engine's, is worth resolving before submission — it would remove a dependency
> with unknown provenance from the shipping tree entirely.

### livekit_ffi

`livekit_ffi.dll` is a statically linked Rust binary built from
[`lifelike-and-believable/livekit-ffi-ue`](https://github.com/lifelike-and-believable/livekit-ffi-ue).
Full provenance, artifact hashes, and the recovered dependency graph are in
[`Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/README.md`](Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/README.md),
with the per-crate license inventory in
[`THIRD_PARTY_NOTICES.md`](Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/THIRD_PARTY_NOTICES.md)
next to it.

Two obligations attach that are **not** satisfied by the MIT declaration alone:

1. **The upstream MIT grant has no license text.** `livekit-ffi-ue` declares
   `license = "MIT"` in `Cargo.toml` but ships no `LICENSE` file — no copyright
   holder, no year, no notice. MIT requires the notice to accompany
   redistributions, so this must be fixed upstream and the file copied into the
   `livekit_ffi/` directory before submission.
2. **The DLL is a combined work.** `livekit`, `livekit-api`, and
   `livekit-protocol` are Apache-2.0 and are statically linked into it with LTO,
   so Apache-2.0 §4 attribution and NOTICE obligations apply to the DLL itself.
   Google's libwebrtc (BSD-3-Clause, plus its own bundled third-party code) is
   also linked in via `webrtc-sys`.

### moq-ffi

`moq_ffi.dll` is likewise a statically linked Rust binary and carries the
obligations of its whole crate graph — 35 crates, all recovered from the binary
and verified against crates.io. See
[`THIRD_PARTY_NOTICES.md`](Source/Open3DTransportMoQ/ThirdParty/moq-ffi/THIRD_PARTY_NOTICES.md).

The heavy obligation here is **AWS-LC**, the crypto backend behind
`aws-lc-rs` / `aws-lc-sys`. Its SPDX string is
`ISC AND (Apache-2.0 OR ISC) AND OpenSSL` — a conjunction, so the OpenSSL terms
apply and are not electable away. The upstream license text is vendored as
[`AWS-LC-LICENSE.txt`](Source/Open3DTransportMoQ/ThirdParty/moq-ffi/AWS-LC-LICENSE.txt).

Two items need attention before submission, both detailed in the notices file:

1. **The OpenSSL and original SSLeay advertising clauses.** Both require an
   acknowledgement string in advertising or documentation that mentions the
   product's features. Whether that has to appear in the Fab listing or in
   in-editor documentation, rather than being satisfied by a shipped notices
   file, is a question for counsel. This is a real obligation, not a
   theoretical one.
2. **mlkem-native is linked but not covered by the vendored LICENSE.** Confirmed
   present in the shipped DLL; needs its own attribution entry
   (`Apache-2.0 OR ISC OR MIT`, © The mlkem-native project authors).

Unlike `livekit_ffi.dll`, this binary was scanned and contains **no** ffmpeg or
OpenH264 — the copyleft blocker above is scoped to the WebRTC transport alone.

---

## 4. Engine plugin dependencies

`Open3DBroadcast.uplugin` declares one plugin dependency:

| Plugin | Source |
| --- | --- |
| LiveLink | Ships with Unreal Engine |

This is an engine-bundled plugin, not a marketplace plugin, so it does not
conflict with the marketplace rule that a submitted plugin must compile without
depending on other marketplace plugins. No LiveLink code is redistributed here.

---

## 5. Build-time-only dependencies (not shipped)

These are git submodules of the parent repository. They are used to build the
o3ds core or are available for optional features, but their code is **not**
present in any binary inside the plugin folder.

| Component | License | Shipped? |
| --- | --- | --- |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later (dual) | **No** |
| libdatachannel | MPL-2.0 | **No** |

### Mbed TLS: evidence that it is not linked

Mbed TLS's dual Apache-2.0 / GPL-2.0-or-later license would require an explicit
election if it shipped. It does not, and this was verified rather than assumed:

- Every binary in the plugin tree was scanned for `mbedtls_*` symbols —
  `flatbuffers.lib`, `opus.lib`, `nng.lib`, `moq_ffi.dll`, `moq_ffi.dll.lib`,
  `livekit_ffi.dll`, `livekit_ffi.dll.lib`. **All seven contain zero matches.**
- `nng.lib` does export the `nng_tls_config_*` / `nng_tls_engine_*` API surface,
  but that is NNG's engine-agnostic TLS *interface*. No TLS engine is registered
  into it, consistent with the `-DNNG_ENABLE_TLS=Off` flag used by
  `Build/Scripts/Sync-O3DSCore.ps1`. Calls into it return `NNG_ENOTSUP`.

**Therefore no license election is required for Mbed TLS.** If TLS is ever
enabled in the NNG transport, this conclusion no longer holds and Apache-2.0
must be elected explicitly before shipping.

---

## Maintaining this file

The two Rust FFI notice files are currently **hand-recovered from binary
inspection** (crate versions were read out of the registry paths embedded in the
DLLs). That is a stopgap, and it will drift silently the next time either DLL is
rebuilt.

The durable fix is to generate them: add a `cargo about generate` (or
`cargo deny`) step to the `livekit-ffi-ue` and `moq-ffi` CI workflows and ship
the generated notice file with each binary drop, then copy it in as part of the
refresh workflow documented in each directory's README.
