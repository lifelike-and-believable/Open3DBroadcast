# AGENTS.md

This is a minimal stub to enable automatic agent instruction discovery at the repository root.
For the full, authoritative rules and workflows, see:

- .github/copilot-instructions.md

Key expectations (short version):
- Verify Unreal Engine API signatures against UE 5.6 docs before use.
- Never block the game thread; networking/encoding runs async.
- FlatBuffers schema is src/o3ds.fbs — regenerate src/o3ds_generated.h after schema changes.
- Prefer deterministic behavior and small, focused changes with tests.
- No hard‑coded credentials/ports/paths in source control.

# Agent Operating Instructions

## Steering via PR comments

- At the start of each iteration:
  1) Run the steering poll script:
     - `scripts/agent/poll-steer.sh` (auto-detects repo and PR)
  2) If the script prints `STOP`, pause and wait for further instructions.
  3) Otherwise, apply the `/steer` directives it prints (oldest-first).
  4) Post a short status comment summarizing what you will do next.

- Ignore comments by bots (`github-actions`, `github-actions[bot]`) and CI messages.
- Do not post `/ue` commands yourself; wait for a human to trigger CI (`/ue quickbuild` or `/ue test`).

## How to run CI (human-triggered)

- Post a comment on this PR:
  - `/ue quickbuild` for a fast editor build
  - `/ue test` for headless automation tests
- The bot replies ✅/❌ with a link to the run.
- Logs artifact: `ue-quick-logs-pr-<PR#>` (uploaded even on failure).