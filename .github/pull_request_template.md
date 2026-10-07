<!--
Title: the work package and the finding ids, for example
"WP-F7: editor module split (ADR 0010; FAB-7, SND-34)". One work package per PR, based on develop.
-->

## What and why

<!-- What changes, the finding or review ids it closes, and why this approach. Name any maintainer decision it relies on, with its date. -->

## Tests

<!-- The tests that failed before the change and pass after it, with the red and green runs (or why the change has no test).
     The commands are in AGENTS.md: the UE build and automation tests for plugin changes, the core tests (CTest) for src/ changes. -->

## Not verified

<!-- Anything that needs a person at the desk: live servers, hardware, takes, a second machine. "None" if nothing. -->

## Checklist

Blocking items (docs/roadmap/plugin-hardening-and-fab-readiness.md, section 7):

- [ ] **Findings:** each finding id in the title was re-read, its evidence no longer holds, and its `Status: closed in #N` line is added.
- [ ] **Threads:** no callback that can run off the game thread holds a raw `this`, owner reference or UObject pointer; `Stop()` and destructors quiesce in-flight callbacks first.
- [ ] **Game thread:** no new blocking call (connect, join, wait, sleep, synchronous HTTP, file I/O) on the game thread.
- [ ] **Untrusted input:** lengths, counts, indices, enums and floats from the wire are range-checked, with a malformed-input test.
- [ ] **UE APIs:** new engine API use cites its UE 5.7 source location.
- [ ] **Tests:** they fail without the change and pass with it; no placeholder assertions, no network tests in the default filter; `Build/automation-test-floors.json` raised by the tests added.
- [ ] **Secrets:** nothing token-like is persisted, logged or committed.
- [ ] **Wire format:** schemas are append-only, generated headers regenerated (not edited) and the core mirror re-synced; version and CHANGELOG "Schema/Protocol" entry per `docs/wire-format.md` section 8.

Also:

- [ ] **Build:** no new warnings; flag-combination builds still pass where flags are touched.
- [ ] **Scope:** no drive-by refactors; behaviour changes and refactors are separate PRs.
- [ ] **Docs:** user-visible changes update the USER_GUIDE, the affected READMEs and `CHANGELOG.md`.
- [ ] **Duplication:** nothing re-creates a shared building block (`Open3DShared`).
