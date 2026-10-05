# Opus (prebuilt, Win64)

Static library and headers for the audio frame codec in `Open3DShared` (`O3DAudioFrameCodec`). The licence is `COPYING` (BSD-3-Clause with royalty-free patent grants). See also `THIRD_PARTY_LICENSES.md`, "Opus".

## Provenance

What the repository can show, and what it cannot:

| Item | Value |
| --- | --- |
| Upstream | https://github.com/xiph/opus |
| Version | **Not recorded.** The library reports `libopus unknown` (built from a checkout without version metadata); the headers use `OPUS_SET_PHASE_INVERSION_DISABLED`, so it is 1.2 or later |
| Commit | **Not recorded** |
| Build flags and toolchain | **Not recorded** |
| Drop date | **Not recorded** (the repository history holds one commit for the file) |

Rebuild `opus.lib` from a pinned upstream tag and write the tag, toolchain and flags in this table, or document that Unreal Engine's own libopus replaces it (open item in `THIRD_PARTY_LICENSES.md`).

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
