# Architecture Decision Records

This folder holds the Architecture Decision Records (ADRs) for Open3DBroadcast. Each ADR records one decision: the problem, the options that were considered, what was chosen and why, and what follows from it.

ADRs are written by the design agent (see [`docs/roadmap/plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §0.1) and need maintainer sign-off before any coding work that depends on them starts.

## Index

| ADR | Title | Plan decision | Status |
|---|---|---|---|
| [0001](0001-platform-scope-first-fab-release.md) | Platform scope for the first Fab release | D1 | Accepted |
| [0002](0002-webrtc-and-moq-in-first-fab-release.md) | WebRTC and MoQ in the first Fab release | D2 | Accepted |
| [0003](0003-core-library-delivery-to-plugin.md) | How the o3ds core library reaches the plugin | D3 | Accepted |
| [0004](0004-credentials-and-secret-transport-options.md) | Credentials and secret transport options | D6 | Accepted |
| [0005](0005-wire-resync-and-loss-contract.md) | Wire-coding resync and loss contract | D7 | Accepted |
| [0006](0006-test-module-layout-and-fakes.md) | Test module layout and fakes | D10 | Accepted |
| [0007](0007-transport-abstraction-and-registry.md) | Transport abstraction, registry and shared transport building blocks | D4 | Accepted |
| [0008](0008-sender-pipeline-threading.md) | Sender pipeline threading | D5 | Accepted |
| [0009](0009-protocol-versioning.md) | Protocol versioning, byte order and wire compatibility | D8 | Accepted |
| [0010](0010-editor-module-split.md) | Editor module split | D9 | Accepted |
| [0011](0011-control-channel.md) | Control channel for events and values | D11 | Accepted |
| [0012](0012-runtime-services-and-global-state.md) | Runtime services and global state | SHR-38 | Proposed |

## Conventions

- **File name:** `NNNN-short-slug.md`, numbered in the order they are written. Numbers are never reused.
- **Status values:** `Proposed` (written, awaiting sign-off), `Accepted` (maintainer signed off), `Superseded by NNNN`, `Rejected`. When an ADR is accepted, update its Status line, the index above, and the matching `**ADR:**` line in the plan.
- **Evidence:** every factual claim about the repository cites `path:line`. Paths starting with `Plugin/` are relative to `ProjectSandbox/Plugins/Open3DBroadcast/`; all other paths are relative to the repository root.
- **Unverified claims:** anything that depends on Unreal Engine 5.7, UnrealBuildTool (UBT), BuildPlugin or Fab behaviour that could not be confirmed from this repository is marked **needs-verification** and repeated under *Open questions*.
- **Findings** are cited by ID (for example `FAB-3`). They live in [`docs/review/2026-09-plugin-review/`](../review/2026-09-plugin-review/README.md).
- An ADR is not edited after acceptance except to change its Status. A changed decision gets a new ADR that supersedes the old one.

## Template

```markdown
# NNNN: <Title>

- **Status:** Proposed (pending maintainer sign-off)
- **Date:** YYYY-MM-DD
- **Plan decision:** Dn in docs/roadmap/plugin-hardening-and-fab-readiness.md §3
- **Related:** <other ADRs, issues>

## Context
What is true today, with path:line evidence. What forces a decision now.

## Decision drivers
The criteria the options are judged against, most important first.

## Options considered
### Option A: <name>
- Pros:
- Cons:
- Cost / effort: S / M / L
- Risk:

## Decision
One clear recommendation, stated so a coding agent can act on it.

## Consequences
What gets easier, what gets harder, which WPs and decisions this enables or constrains.

## Implementation outline
Ordered steps mapped to WP IDs, with the files each step changes.

## Verification / acceptance
How a reviewer confirms the decision was implemented correctly.

## Open questions for the maintainer
Numbered, including every needs-verification item.

## References
Finding IDs, file paths, external sources (with retrieval date).
```
