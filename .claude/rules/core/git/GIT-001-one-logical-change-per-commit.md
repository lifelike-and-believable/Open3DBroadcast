---
id: GIT-001
title: One logical change per commit and pull request
level: SHOULD
scope: core
verified-by: [review]
targets-failure: scope-creep
observed-on: []
rationale: Small, single-purpose changes are easier to review, revert, and evaluate.
sources: ["practices/working-with-agents.md"]
---
Keep each commit and pull request to one logical change. Write commit messages that say what changed and why. Keep unrelated fixes you notice out of the change and list them in your report instead.
