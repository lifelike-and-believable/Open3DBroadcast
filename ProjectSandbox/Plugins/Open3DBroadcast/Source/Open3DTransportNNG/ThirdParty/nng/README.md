# NNG (prebuilt, Win64)

Static library for the NNG transport (`Open3DTransportNNG`). The licence is `LICENSE.txt` (MIT).

## Provenance

What the repository can show, and what it cannot:

| Item | Value |
| --- | --- |
| Upstream | https://github.com/nanomsg/nng |
| Version | 1.3.0 (`NNG_MAJOR_VERSION`, `NNG_MINOR_VERSION`, `NNG_PATCH_VERSION` in `include/nng/nng.h`) |
| Built by | the Open3DStream repository's self-hosted Windows runner: `nng.lib` embeds the object paths `...\Open3DStream\Open3DStream\thirdparty\build\nng\src\nng.dir\Release\...`, so it is a Release (MSVC) build of that repository's `thirdparty/nng` submodule |
| Submodule pin of that checkout | **Not recorded.** The current pin of `thirdparty/nng` in this repository is `3fe636344e0d09d7194687fa70ff3881476c27af`; whether this binary was built from it is unconfirmed |
| Build flags | **Not recorded** (the TLS API symbols are present in the library; whether TLS was enabled is unconfirmed) |
| Drop date | **Not recorded** (the repository history holds one commit for the file) |

Fill in the three unrecorded rows from the build that produced the file, or replace the file with a build whose provenance is written down here.

## Artifact inventory (Win64)

| Relative Path | Description | SHA256 |
| --- | --- | --- |
| `lib/Win64/nng.lib` | Static library linked by `Open3DTransportNNG.Build.cs` | `FBC0A1E55634D18EA24C032E026DFC296231AB6C0B89492C802E188A99A3A6A9` |
| `include/nng/` | NNG headers | (text files, not hashed) |

`Build/Scripts/check-third-party-binaries.py` (run in CI) fails when the hash above differs from the tracked file.

## Refreshing

1. Build NNG for Win64 Release from a pinned upstream tag or commit.
2. Copy the library and headers here, keeping the `include/` and `lib/Win64/` layout.
3. Update the provenance table (version, commit, build flags, date) and the SHA256 above in the same commit.
4. Build the plugin once to confirm `Open3DTransportNNG.Build.cs` finds the library.
