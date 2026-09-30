# Open3DBroadcast plugin: shared third-party code

Everything the plugin compiles or links lives under `Source/`, so `RunUAT BuildPlugin` and the Fab source zip carry it without a `Config/FilterPlugin.ini` entry (WP-F1, FAB-2). This folder holds the third-party code that is not owned by a single module. It has no `.Build.cs`, so UnrealBuildTool does not treat it as a module.

## Open3DStreamCore/ (generated)

The Open3DStream core (`src/o3ds` in the repository), compiled by the `Open3DStreamCore` module (`Source/Open3DStreamCore/`). See [ADR 0003](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0003-core-library-delivery-to-plugin.md).

- **Generated; do not edit.** `Build/Scripts/sync_o3ds_core.py` copies the include closure of the headers listed in `Build/o3ds-core-manifest.txt`, the committed `src/o3ds_generated.h`, the FlatBuffers runtime headers (`thirdparty/flatbuffers` pin) and CRCpp's `CRC.h` (`thirdparty/crccpp` pin). `SYNC_STAMP.txt` records the pins and hashes. CI runs the script with `--check` and fails when this folder differs from `src/`.
- **Licenses:** `LICENSES/` holds the Open3DStream (MIT), FlatBuffers (Apache-2.0) and CRC++ (BSD-3-Clause) texts.
- **Used by:** every module that includes an `o3ds/...` header, through a dependency on `Open3DStreamCore`.

## opus/

- **Library:** Opus audio codec, prebuilt `lib/Win64/opus.lib` and its headers. Version and provenance: see `THIRD_PARTY_LICENSES.md`, "Opus".
- **License:** BSD-3-Clause with royalty-free patent grants (`opus/COPYING`).
- **Used by:** `Open3DShared` (`O3DAudioFrameCodec`). On platforms without the library, `Open3DShared` builds with `O3D_WITH_OPUS=0`.

## Module-specific third-party code

Libraries used by one module live in that module's own `ThirdParty/` folder:

- `Source/Open3DTransportNNG/ThirdParty/nng/`: NNG, prebuilt Win64 static library.
- `Source/Open3DTransportMoQ/ThirdParty/moq-ffi/`: MoQ FFI DLL.
- `Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/`: LiveKit FFI DLL.
