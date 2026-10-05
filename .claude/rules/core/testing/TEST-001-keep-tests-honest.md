---
id: TEST-001
title: Keep tests honest
level: MUST
scope: core
paths: ["**/*.test.*", "**/*.spec.*", "**/test/**", "**/tests/**", "**/Tests/**", "**/__snapshots__/**"]
verified-by: [hook, ci]
check: "hook: guard-test-edits (asks before any test-file edit); ci: test-change-guard (flags PRs that change tests alongside implementation)"
targets-failure: test-gaming
observed-on: [claude-opus-4-1, claude-opus-4-6, claude-opus-4-7, claude-sonnet-4-6]
rationale: Claude models game tasks mainly by editing tests, and an anti-hack prompt still leaves 12.5 to 37.5% residual cheating on impossible tasks.
sources: ["practices/testing-and-code-quality.md", "docs/research/findings.md", "docs/research/notes/verification_of_secondary_figures.md"]
---
<!-- Worked example for docs/rule-format.md. Enforcement lands in Phase 2 (hook) and Phase 6 (CI). -->
Make failing tests pass by changing the implementation. Leave existing assertions, expected values, snapshots, and skip markers as they are, because a weakened test hides the defect it was written to catch.

If a test looks wrong, or the task cannot be done without changing a test, stop and say which test and why instead of editing it. Add new tests freely.

When you change a test file for any reason, list each changed test and the reason in your final report.
