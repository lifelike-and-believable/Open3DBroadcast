# livekit_ffi Vendored Artifacts

This directory contains the prebuilt **livekit_ffi** package used by the
`Open3DTransportWebRTC` module. It is a thin C ABI shim (`lk_*` functions, see
`include/livekit_ffi.h`) over the [LiveKit Rust SDK](https://github.com/livekit/rust-sdks),
which in turn statically links Google's libwebrtc. It lets Unreal C++ join a
LiveKit room and publish/subscribe mocap frames (data channel) and audio (Opus)
without pulling a WebRTC stack into the Unreal build.

> ⚠️ **This is not LiveKit's own `livekit-ffi` crate.** Upstream LiveKit ships a
> protobuf-request-based FFI used by their Node/Python/Unity bindings. The
> library here is a separate, purpose-built C ABI wrapper maintained for this
> plugin. Do not substitute one for the other — the ABIs are unrelated.

## Upstream Provenance

| Item | Value |
| --- | --- |
| Repository | https://github.com/lifelike-and-believable/livekit-ffi-ue (crate directory `livekit_ffi/`) |
| Release tag | **TBD — see "Provenance gap" below** |
| Commit | **TBD — see "Provenance gap" below** |
| Build host | GitHub Actions `windows-latest`, via `.github/workflows/build-ffi.yml` |
| Build profile | `cargo build --release --features with_livekit`, target `x86_64-pc-windows-msvc`, `lto = true` |
| Toolchain | rustc 1.87.0 (pinned by the workflow) |
| LiveKit SDK | `livekit` 0.7.24 / `livekit-api` 0.4.9 / `livekit-protocol` 0.5.1 (all `=`-pinned upstream) |
| WebRTC | `libwebrtc` 0.3.19 via `webrtc-sys` 0.3.16 (statically linked) |
| Wrapper license | MIT (declared in `Cargo.toml`; see caveat under Licensing) |

The build host and toolchain above were **recovered from the shipped binaries**
and then confirmed against the upstream repository:

- `livekit_ffi.pdb` embeds the compile-time source root
  `D:\a\livekit-ffi-ue\livekit-ffi-ue\livekit_ffi\target\release\...`. The
  `D:\a\<repo>\<repo>` shape is the GitHub Actions Windows checkout convention,
  which is what identified the repository name and the CI build host.
- `livekit_ffi.dll` embeds `rustc version 1.87.0` and per-crate registry paths
  under `C:\Users\runneradmin\.cargo\registry\src\index.crates.io-*`, which is
  what pins the dependency versions listed in `THIRD_PARTY_NOTICES.md`.
- The upstream workflow, its pinned toolchain, and the `=0.7.24` livekit pin all
  match those recovered values.

### Provenance gap

The upstream repository is identified, but the **exact release/commit these
specific binaries came from is still unrecorded**, so the artifact cannot yet be
reproduced from source. `livekit-ffi-ue` publishes per-release Windows assets
named `livekit-ffi-plugin-windows-x64-v*.zip` whose internal layout matches this
directory, so the DLL here was most likely taken from one of those rather than
built locally. To close the gap:

1. Download the candidate release assets and compare against the SHA256 values
   in the inventory below — a byte match identifies the release outright.
2. Record the release tag, commit SHA, and workflow run URL in the table above.
3. Add `livekit-ffi-ue` as a git submodule (as `ProjectSandbox/External/moq-ffi`
   already is for the MoQ transport), so the source is pinned alongside the
   binary rather than only referenced by name.
4. Copy the upstream `LICENSE` into this directory once it exists (see below).

## Artifact Inventory (Win64)

| Relative Path | Description | SHA256 |
| --- | --- | --- |
| `bin/Win64/livekit_ffi.dll` | Runtime DLL loaded by the WebRTC transport | `E102555EA3BF1B27FFE220AF220786E0FF652D7D4656D257394BDB276ED16DFE` |
| `bin/Win64/livekit_ffi.pdb` | Debug symbols for crash triage | `257A21CD4300987A043CD5C1A8D9E734C2B949009141BE10E866699E02C952C1` |
| `lib/Win64/livekit_ffi.dll.lib` | Import library linked at build time | `30A6A5DB6D21E4D39F586AF5F267252367438B8712E49C9BB7D2B194DD041DFD` |
| `include/livekit_ffi.h` | C API header | (text file, not hashed) |

Only Win64 artifacts are shipped. `Open3DTransportWebRTC.Build.cs` throws if the
`.lib`, header, or DLL is missing, so other platforms currently cannot build this
module.

## Licensing

`livekit_ffi.dll` is a statically linked Rust binary: it carries the license
obligations of roughly 60 crates plus Google's libwebrtc, none of which are
visible from the DLL itself. The recovered dependency list, with per-crate
license identifiers, is in `THIRD_PARTY_NOTICES.md` next to this file, and is
summarised in the plugin-level `THIRD_PARTY_LICENSES.md`.

Two things to be aware of:

- **The wrapper's MIT grant has no license text.** `livekit-ffi-ue` declares
  `license = "MIT"` in `Cargo.toml` and repeats "MIT" in its README, but the
  repository contains no `LICENSE` file — no copyright holder, no year, no
  notice text. MIT requires the notice to travel with redistributions, so this
  should be fixed upstream and the resulting file copied here as `LICENSE`.
- **The DLL is a combined work.** Because `livekit`, `livekit-api`, and
  `livekit-protocol` are Apache-2.0 and are statically linked (with LTO) into
  the shipped DLL, Apache-2.0 §4 attribution and NOTICE obligations attach to
  redistributing it regardless of the wrapper's MIT declaration.

**The authoritative notice file should be generated at build time**, not
maintained by hand. Add a `cargo about generate` (or `cargo deny`) step to the
`livekit-ffi-ue` CI workflow and ship its output alongside the DLL; the
hand-recovered list here is a stopgap derived from binary inspection and can
drift silently when the crate graph changes.

## Refresh Workflow

1. Check out the desired `livekit-ffi-ue` commit.
2. Build for Win64:
   `cargo build --release --features with_livekit --target x86_64-pc-windows-msvc`.
   The `with_livekit` feature is required — it is what the shipped artifacts were
   built with (see the provenance table above), and omitting it produces a
   different DLL.
3. Copy `livekit_ffi.dll`, `livekit_ffi.pdb`, `livekit_ffi.dll.lib`, and the
   generated header into the `bin/Win64/`, `lib/Win64/`, and `include/` layout
   used here.
4. Update the provenance table (commit, toolchain, SDK versions) **and** the
   SHA256 column above.
5. Regenerate `THIRD_PARTY_NOTICES.md` from the new build's crate graph.
6. Run an Unreal build to confirm `Open3DTransportWebRTC.Build.cs` resolves the
   new files and that the DLL loads at runtime.
7. Commit the binaries, this README, and the notices file together.

## Troubleshooting

- **Build failure:** `Open3DTransportWebRTC.Build.cs` throws if the `.lib`, DLL,
  or include directory is missing. Re-run step 3.
- **Runtime failure:** check `LogOpen3DTransportWebRTC`. A load failure with
  exports missing usually means the DLL and `livekit_ffi.h` are from different
  builds — refresh both together, never one alone.
- **Platform support:** Win64 only today.
