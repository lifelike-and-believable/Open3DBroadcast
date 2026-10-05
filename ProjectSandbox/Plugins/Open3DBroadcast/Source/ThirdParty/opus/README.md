# Opus (prebuilt, Win64)

Static library and headers for the audio frame codec in `Open3DShared` (`O3DAudioFrameCodec`). The licence is `COPYING` (BSD-3-Clause with royalty-free patent grants). See also `THIRD_PARTY_LICENSES.md`, "Opus".

## Provenance

Verified 2026-10-05 by downloading the release asset and comparing it with the tracked file:

| Item | Value |
| --- | --- |
| Upstream | https://github.com/xiph/opus |
| Built from | the fork https://github.com/lifelike-and-believable/opus, tag `opus-win-v.1.0.0`, commit `697c670c8acc52ae3894a8c476dda551ba33d189` (2025-10-27) |
| Source version | An upstream development snapshot, not a release: the fork's last upstream commit is `43efb99765aeed17eb9066f4eaedcf4f4081eb3b` (2025-10-07, "Reduce SILK encoder state size for mono"). The library reports `libopus unknown` because the build had no version metadata, so no release number is recorded |
| Build | The fork's `windows-self-hosted.yml` workflow: CMake, Visual Studio 17 2022, x64, Release, static (`-DOPUS_BUILD_SHARED_LIBRARY=OFF -DBUILD_SHARED_LIBS=OFF`) |
| Release asset | `opus-Windows-Lib-X64-Release-opus-win-v.1.0.0.zip` (SHA256 `6DC9E2522270CCE63187E8B699D3D2539B903839F8E982BA762EBB3132B0A068`); its `lib/opus.lib` is byte-identical to the tracked file |
| Headers | **Do not match the library.** The tracked `include/` differs from the release asset's `include/` in 5 of 6 headers beyond line endings (311 changed lines). The release's `opus.h` declares `opus_encode24`, which the tracked one lacks, so the headers come from an older Opus than the library. The plugin only calls the long-standing encode/decode API, which has not been compiled against the newer headers; replace `include/` with the release's after a plugin build and test |

## Open items

- Pin the Opus source (for example as a submodule or a recorded tag) instead of a development snapshot, or switch to a tagged Opus release.
- Decide whether to take the release's headers (see "Headers" above).

## Artifact inventory (Win64)

| Relative Path | Description | SHA256 |
| --- | --- | --- |
| `lib/Win64/opus.lib` | Static library linked by `Open3DShared.Build.cs` | `8DCB5D7B0D8FC4BF6D42F9795B2B251D6A4FE1E43350989B4DC6ACD139308A2A` |
| `include/` | Opus headers | (text files, not hashed) |

`Build/Scripts/check-third-party-binaries.py` (run in CI) fails when the hash above differs from the tracked file.

## Refreshing

1. Build Opus for Win64 Release from a pinned upstream tag.
2. Copy the library and headers here, keeping the `include/` and `lib/Win64/` layout.
3. Update the provenance table and the SHA256 above in the same commit.
4. Build the plugin once to confirm `Open3DShared.Build.cs` finds the library.
