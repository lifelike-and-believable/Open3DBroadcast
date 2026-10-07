# 0014: Supported engine versions (UE 5.7 and 5.8)

- **Status:** Accepted (maintainer sign-off 2026-10-07)
- **Date:** 2026-10-07
- **Plan:** [`docs/roadmap/engine-version-5.8-plan.md`](../roadmap/engine-version-5.8-plan.md); spike: [`engine-version-5.8-spike.md`](../roadmap/engine-version-5.8-spike.md)
- **Amends:** [ADR 0001](0001-platform-scope-first-fab-release.md)'s engine scope (UE 5.7 only). Its platform decision, Win64 only, stays.

## Context

Both plugins build for UE 5.7 only: the source descriptors say `"EngineVersion": "5.7.0"`
(`Plugin/Open3DBroadcast.uplugin:3`, `ProjectSandbox/Plugins/Open3DBroadcastWebRTC/Open3DBroadcastWebRTC.uplugin:3`),
the Target.cs files pin `BuildSettingsVersion.V6` and `EngineIncludeOrderVersion.Unreal5_7` (rule
O3D-001), and every UE workflow hard-codes `UE_5.7` as `UE_ROOT`. UE 5.8.2 is released. The
2026-10-06 plan weighed 5.6, 5.7 and 5.8; the compile spike of 2026-10-07 built both plugins on
5.8.2 with CI's flags: the main plugin passes 493 of its 494 tests, and the work found is three small
fixes (two deprecated APIs and one test bug).

## Decision drivers

1. Users on the current engine can install the plugin from Fab.
2. One source tree, no per-engine branches to keep in step.
3. CI cost on the single self-hosted runner.
4. No toolchain the runner does not already have.

## Options considered

- **5.6, 5.7 and 5.8.** Widest reach. 5.6 needs a VS 2022 toolset on the runner (it has VS 2026
  only), cannot compile the current Target.cs settings, and 5.8 already marks 5.6's include order
  unsupported in 5.9. Rejected.
- **5.7 and 5.8.** Same toolchain as today, small code delta (spike). Chosen.
- **5.8 only.** Drops users who stay on 5.7 for the length of a production. Rejected.

## Decision

1. **Engines:** UE 5.7 and UE 5.8, Win64 only. 5.7 stays the main development engine. 5.6 is not
   supported.
2. **Policy:** support the latest minor versions Fab accepts, two at a time; when a new minor version
   ships, add it and drop the oldest, each in its own ADR.
3. **Source:** one tree. Version differences go in `Open3DShared/Public/O3DEngineCompat.h` on top of
   `Misc/EngineVersionComparison.h`, each guard with a comment naming the engine change. Code that
   compiles cleanly on both engines is preferred to a guard.
4. **WebRTC add-on:** follows the same engines; it is built against the main plugin's package for each
   engine.
5. **`EngineVersion`:** removed from both source `.uplugin` files. Packaging stamps it for the engine
   that built the package (BuildPlugin does this for the main plugin; the scripts do it for the
   add-on and the Fab zip), and the test script fails when a project plugin's `EngineVersion` does not
   match the engine. Rule O3D-005 changes accordingly.
6. **PR CI:** the full UE job on 5.7, plus a reduced job on 5.8 (build with warnings as errors, the
   tests, the add-on and its tests) on every non-draft PR, both behind "Plugin CI result". If the
   queue grows too long, 5.8 moves to the nightly, the release and manual runs.
7. **Fab:** one listing with one source zip per engine. A person confirms Fab's current
   multi-version rules on the live pages and uploads each zip.

Target.cs files select their settings by engine: `#if UE_5_8_OR_LATER` → `BuildSettingsVersion.V7` and
`EngineIncludeOrderVersion.Unreal5_8`; otherwise V6 and `Unreal5_7`. Rule O3D-001 says this.

## Consequences

- Every plugin PR is built and tested on two engines once the 5.8 CI leg exists; the UE queue grows
  by about the reduced job's length.
- The self-hosted runner needs UE 5.8 installed (Win64 target only) before the CI matrix can land.
- Local development uses one git worktree per engine, because `Binaries/` and `Intermediate/` are
  per-engine output.
- Releases produce one zip per engine for each plugin.

## Implementation outline

1. WP-V3: the three spike fixes (`OnPostEngineInit`, `PLATFORM_64BITS`, the capture test's path).
2. WP-V2: Target.cs guards, rules O3D-001 and O3D-005, `EngineVersion` removal and stamping, script
   parameters and the engine check in `Run-AutomationTests.ps1`.
3. WP-V8a (desk): install UE 5.8 on the runner.
4. WP-V4: the CI matrix. WP-V5: the release matrix. WP-V6 (partly desk): LiveLink timing on 5.8 and
   a live take. WP-V7: docs. WP-V8b (desk): Fab.

## Verification / acceptance

- Both plugins build with `-FailOnWarnings` and pass every test on 5.7 and on 5.8, locally and, after
  WP-V4, in CI.
- A package built on each engine carries that engine's `EngineVersion`.
- The maintainer's live take on 5.8 (WP-V6).

## Open questions for the maintainer

1. **needs-verification:** Fab's current rules for one listing with several engine versions.
2. **needs-verification:** UE 5.8's LiveLink EngineTime change (read time from `Time.CurrentTime`)
   against ADR 0013's receiver arithmetic; WP-V6.

## References

- Plan and spike above; ADR 0001; ADR 0013; rules O3D-001 and O3D-005.
