---
id: DOC-001
title: Update the docs your change makes wrong
level: SHOULD
scope: core
verified-by: [review]
targets-failure: convention-drift
observed-on: []
rationale: Instructions and docs that describe old behaviour mislead both people and later agent sessions.
sources: ["practices/working-with-agents.md"]
---
When your change alters a command, setting, interface, or behaviour that AGENTS.md, a README, or API documentation describes, update that text in the same change. Do not edit tool-managed blocks, such as the Next.js `nextjs-agent-rules` section of AGENTS.md.
