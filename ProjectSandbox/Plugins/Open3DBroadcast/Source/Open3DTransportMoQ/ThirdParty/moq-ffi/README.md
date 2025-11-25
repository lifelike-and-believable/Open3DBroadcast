# moq-ffi Vendored Artifacts

This directory contains the prebuilt **moq-ffi** package used by the `Open3DTransportMoQ` module. The binaries bridge Unreal Engine C++ code to Cloudflare's [moq-rs](https://github.com/cloudflare/moq-rs) implementation of the Media over QUIC (MoQ) transport.

> ℹ️ For full production-readiness details, build notes, and diagnostic procedures see `ProjectSandbox/External/moq-ffi/README.md`.

## Upstream Provenance

| Item | Value |
| --- | --- |
| Repository | https://github.com/lifelike-and-believable/moq-ffi |
| Commit | `567933e82c780b157705b64fb84729ae46b534ca` |
| Branch | `draft-ietf-moq-transport-07` (MoQ draft-07 compatibility) |
| Build Profile | `cargo build --release --features with_moq_draft07` on Win64/MSVC |
| Drop Date | 2025-11-24 |

Only Win64 artifacts are currently shipped. Linux/macOS placeholders exist so future drops have well-known paths, but those platforms will automatically disable MoQ (see `O3D_WITH_TRANSPORT_MOQ.md`).

## Artifact Inventory (Win64)

| Relative Path | Description | SHA256 |
| --- | --- | --- |
| `bin/Win64/Release/moq_ffi.dll` | Runtime DLL loaded by `FMoQFfiSupport` | `EF248AABDD4F9329C6F874261E6BF2CC9DCF62170CD95CD2F3C79BEC4B1237EF` |
| `bin/Win64/Release/moq_ffi.pdb` | Debug symbols for crash triage | `66F612E8344037D10D0F2D8107F3AF0C4F9F426D0903578DAE4C3DF39E06A318` |
| `lib/Win64/Release/moq_ffi.dll.lib` | Import library linked at build time | `B3377B79C3DB3D3047C2FA352C1B10D3C87AA548378407A01299CB4A4A277B1A` |
| `include/moq_ffi.h` | C API header emitted by `cbindgen` | (text file, not hashed—see upstream repo) |

## Refresh Workflow

1. Clone or update the upstream repository under `ProjectSandbox/External/moq-ffi`.
2. Checkout the desired commit (see commit above for current drop).
3. Build artifacts for the required platforms. For Win64:
   ```powershell
   cd ProjectSandbox/External/moq-ffi/moq_ffi
   cargo build --release --features with_moq_draft07
   pwsh ../tools/package-plugin.ps1 -CrateDir . -OutDir ../../artifacts/plugin-windows-x64
   ```
4. Copy the packaged contents into this directory, preserving the `include/`, `lib/<Platform>/Release/`, and `bin/<Platform>/Release/` layout.
5. Update the table above with the new commit SHA, build flags, and **SHA256 hashes** for each binary.
6. Run the Unreal build once to ensure `Open3DTransportMoQ.Build.cs` can locate the new files and that `FMoQFfiSupport` loads them at runtime.
7. Commit the updated binaries and this README together.

## Troubleshooting Checklist

- **Build Failure:** `Open3DTransportMoQ.Build.cs` throws if the `.lib` or DLL is missing. Re-run step 4.
- **Runtime Failure:** Check `LogMoQFfiSupport` output. If validation fails, confirm the hashes match the table above and that the DLL exports match the current header.
- **Platform Support:** Only Win64 binaries are available today. Linux/macOS builds will see `O3D_WITH_TRANSPORT_MOQ` auto-disable until we vendor matching artifacts.

For deeper debugging guidance (panic handling, async dispatcher expectations, Cloudflare relay integration, etc.) consult the documentation in `ProjectSandbox/External/moq-ffi`.
