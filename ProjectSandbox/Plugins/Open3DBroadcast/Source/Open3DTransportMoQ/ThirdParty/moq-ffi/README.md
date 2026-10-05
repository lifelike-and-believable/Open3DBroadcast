# moq-ffi Vendored Artifacts

This directory contains the prebuilt **moq-ffi** package used by the `Open3DTransportMoQ` module. The binaries bridge Unreal Engine C++ code to Cloudflare's [moq-rs](https://github.com/cloudflare/moq-rs) implementation of the Media over QUIC (MoQ) transport.

> ℹ️ For full production-readiness details, build notes, and diagnostic procedures see `ProjectSandbox/External/moq-ffi/README.md`.

## Upstream Provenance

| Item | Value |
| --- | --- |
| Repository | https://github.com/lifelike-and-believable/moq-ffi |
| Release tag | `v0.1.7` |
| Commit | `f0f250148be450cad1d37bf18207e920b534e472` (the same commit `ProjectSandbox/External/moq-ffi` pins) |
| Branch | `draft-ietf-moq-transport-07` (MoQ draft-07 compatibility) |
| Build Profile | `cargo build --release --features with_moq_draft07` on Win64/MSVC |
| Drop Date | 2025-11-24 |

Only Win64 artifacts are shipped, and the module is Win64-only (`PlatformAllowList` in `Open3DBroadcast.uplugin`, ADR 0001). If the module is ever configured for another platform, `O3D_WITH_TRANSPORT_MOQ` is forced to 0 and it builds as a stub. `Open3DTransportMoQ.Build.cs` has no Linux or macOS paths (BUILD-2); add them together with the binaries.

## Artifact Inventory (Win64)

| Relative Path | Description | SHA256 |
| --- | --- | --- |
| `bin/Win64/Release/moq_ffi.dll` | Runtime DLL loaded by the module through `FO3DFfiLibrary` (Open3DShared) | `BB722F4475C814C4764CC97A126379D2DAF37285487B0810B4FA293CB8BF46FF` |
| `moq_ffi.pdb` (not in the tree; release asset, see `Build/README.md`, "Debug symbols") | Debug symbols for crash triage | `B51E58C9361680E3FAC05826006299CB2AA9A76005A6BF5643C5B1021BBD8B32` |
| `lib/Win64/Release/moq_ffi.dll.lib` | Import library linked at build time | `B3377B79C3DB3D3047C2FA352C1B10D3C87AA548378407A01299CB4A4A277B1A` |
| `include/moq_ffi.h` | C API header emitted by `cbindgen` | (text file, not hashed—see upstream repo) |

> **Provenance verified 2026-10-05.** The tracked `moq_ffi.dll` and `moq_ffi.dll.lib` and `include/moq_ffi.h` are byte-identical (header modulo line endings) to the contents of `moq-ffi-plugin-windows-x64.zip` in release `v0.1.7` (zip SHA256 `1E94E0D7C1194C7452DD17693183F68571AA7358FF64EEB6DE365CD7058FA77C`), built by the repository's `build-ffi.yml` on `windows-latest` (Rust 1.87.0). Commit `f0f2501...` is dated 2025-11-24, the drop date above. The commit (`567933e...`), DLL hash (`EF248AAB...37EF`) and PDB hash (`66F612E8...A318`) this README listed before were replaced: the DLL hash matches neither the tracked DLL nor the `v0.1.5`, `v0.1.7`, `v0.1.8` or `v0.1.9` release DLLs (`v0.1.6` has no asset), and the PDB hash differs from the `v0.1.7` PDB. Newer releases exist (`v0.1.8`, `v0.1.9`); this tree has not moved to them.

## Refresh Workflow

1. Clone or update the upstream repository under `ProjectSandbox/External/moq-ffi`.
2. Checkout the desired commit (see commit above for current drop).
3. Build artifacts for the required platforms. For Win64:
   ```powershell
   cd ProjectSandbox/External/moq-ffi/moq_ffi
   cargo build --release --features with_moq_draft07
   pwsh ../tools/package-plugin.ps1 -CrateDir . -OutDir ../../artifacts/plugin-windows-x64
   ```
4. Copy the packaged contents into this directory, preserving the `include/`, `lib/<Platform>/Release/`, and `bin/<Platform>/Release/` layout. Do not commit `moq_ffi.pdb`: record its SHA256 below and attach it to the next release (`Build/README.md`, "Debug symbols"). CI fails if a `.pdb` is tracked.
5. Update the table above with the new commit SHA, build flags, and **SHA256 hashes** for each binary.
6. Run the Unreal build once to ensure `Open3DTransportMoQ.Build.cs` can locate the new files and that the module loads and validates them at runtime (`FMoQFfiSupport::ValidateLibrary`).
7. Commit the updated binaries and this README together.

## Troubleshooting Checklist

- **Build Failure:** `Open3DTransportMoQ.Build.cs` throws if the `.lib` or DLL is missing. Re-run step 4.
- **Runtime Failure:** Check `LogMoQFfiSupport` output. If validation fails, confirm the hashes match the table above and that the DLL exports match the current header.
- **Platform Support:** Only Win64 binaries are available today. The module is not built for other platforms (see above).

For deeper debugging guidance (panic handling, async dispatcher expectations, Cloudflare relay integration, etc.) consult the documentation in `ProjectSandbox/External/moq-ffi`.
