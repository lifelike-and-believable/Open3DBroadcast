---
id: WA-005
title: Ground every claim in this session's evidence
level: SHOULD
scope: core
verified-by: [review]
targets-failure: unverified-claim
observed-on: [claude-fable-5]
rationale: Asking the model to audit each claim against a tool result from the session nearly eliminated fabricated status reports in Anthropic's testing.
sources: ["practices/working-with-agents.md", "docs/research/notes/instruction_design.md"]
---
Before reporting progress or results, check each claim against a tool result from this session. State plainly what you did not do or could not verify. Say "I did not run the tests" rather than implying they pass.
