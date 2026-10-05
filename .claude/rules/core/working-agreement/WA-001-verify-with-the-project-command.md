---
id: WA-001
title: Verify with the project command and show the output
level: MUST
scope: core
verified-by: [ci]
check: "ci: the project's verify workflow runs the same command on every pull request"
targets-failure: unverified-claim
observed-on: [claude-opus-4-6, claude-sonnet-4-6]
rationale: Agents report success without running checks; a named command gives a concrete check, and generic "verify" instructions cause over-verification on Opus 5 and later.
sources: ["practices/working-with-agents.md", "docs/research/findings.md", "docs/research/recommendations.md#b-working-agreement-phase-2"]
---
Before you report a code change as done, run the verification command listed in AGENTS.md and include its final output in your report. If it fails, say so and show the failing part rather than describing the work as complete.

If AGENTS.md lists no verification command, say that in your report and name the checks you did run.
