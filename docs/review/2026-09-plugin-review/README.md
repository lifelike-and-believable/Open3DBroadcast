# Open3DBroadcast plugin review: findings (September 2026)

**Baseline:** `develop` @ `7aea235` · **Date:** 2026-09-29

This folder holds the detailed findings from a full review of the Open3DBroadcast Unreal plugin, the `src/o3ds` core library, and the build, CI, packaging and documentation around them. The implementation plan built from these findings is [`docs/roadmap/plugin-hardening-and-fab-readiness.md`](../../roadmap/plugin-hardening-and-fab-readiness.md). Its §8 assigns every finding ID below to exactly one work package.

| File | Scope | ID prefix | Findings |
|---|---|---|---:|
| [sender.md](sender.md) | `Source/Open3DSender` | SND | 37 |
| [receiver.md](receiver.md) | `Source/Open3DReceiver` | RCV | 34 |
| [shared.md](shared.md) | `Source/Open3DShared`, plugin `ThirdParty/` wiring | SHR | 38 |
| [transports-sockets-loopback-nng.md](transports-sockets-loopback-nng.md) | `Open3DTransportSockets`, `Open3DTransportLoopback`, `Open3DTransportNNG` | TRB | 47 |
| [transports-webrtc-moq.md](transports-webrtc-moq.md) | `Open3DTransportWebRTC`, `Open3DTransportMoQ` | TRF | 40 |
| [core-library.md](core-library.md) | `src/o3ds`, schema, core tests, core CMake | CORE | 30 |
| [fab-ci-docs.md](fab-ci-docs.md) | `.uplugin`, Build.cs, third-party licensing, CI, docs, UX | FAB, CI, BUILD, HYG, DOC, UX, LIC | 48 |
| [poc/](poc/) | ASan reproducers for CORE-1 and CORE-2, plus CRC and hierarchy benchmarks | | |

## Finding format

Each finding has a **Category**, **Severity** (critical, high, medium, low), **Location** (file:line), **Evidence**, **Recommendation**, **Effort** (S, M, L) and **Owner** (design, coding, review, docs).

- **Paths:** each file states at the top which directory its paths are relative to.
- **Unverified behaviour:** findings marked *needs-UE-verification*, *needs-FFI-verification* or *needs NNG-doc verification* depend on behaviour that could not be confirmed from this repository. Check them against UE 5.7 source or the relevant library before acting.
- **Closing a finding:** add a line `- Status: closed in #<PR>` under the finding.

## How the findings were verified

- **Core library:** built and tested with GCC 13 (Debug, ASan and UBSan). 174 of 174 existing tests pass. CORE-1 (`./poc calc`) and CORE-2 (`./poc frag`) reproduce under AddressSanitizer. The harness sources are in `poc/`, and they build against `open3dstreamstatic` plus the flatbuffers and CRCpp headers.
- **UE plugin:** read-only review. Nothing was compiled against UE 5.7. A sample of critical and high findings was re-checked by hand against the source (the list is in the plan's §1.2).
- **Fab requirements:** the official pages could not be fetched from the review environment. The sources used are listed in `fab-ci-docs.md`, and plan work package WP-F0 re-verifies them.
- **Redaction:** one public IP address quoted from the NNG README has been redacted here (see TRB-44).
