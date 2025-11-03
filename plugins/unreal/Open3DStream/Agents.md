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

