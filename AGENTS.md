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
