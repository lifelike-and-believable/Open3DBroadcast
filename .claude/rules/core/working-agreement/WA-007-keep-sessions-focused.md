---
id: WA-007
title: Keep sessions focused
level: MAY
scope: core
verified-by: [review]
targets-failure: convention-drift
observed-on: [claude-sonnet-4-6]
rationale: Rule adherence declines as sessions grow, and repeated failed corrections fill context with wrong attempts.
sources: ["practices/working-with-agents.md", "docs/research/findings.md"]
---
Use a subagent for broad exploration so the main session keeps only its conclusions. If the same problem has failed two corrections, stop, summarize what was tried and what you learned, and suggest continuing in a fresh session with that summary.
