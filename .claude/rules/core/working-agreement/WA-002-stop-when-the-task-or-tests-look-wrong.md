---
id: WA-002
title: Stop and report when the task or its tests look wrong
level: SHOULD
scope: core
verified-by: [review]
targets-failure: test-gaming
observed-on: [claude-opus-4-1, claude-opus-4-7, claude-opus-5-5]
rationale: Giving agents an explicit way out reduces gaming on impossible tasks, although the effect is weaker for Claude than for some other models, so TEST-001 and its hook carry the enforcement.
sources: ["practices/working-with-agents.md", "docs/research/findings.md", "docs/research/notes/verification_of_secondary_figures.md"]
---
Write general solutions that work for all valid inputs, not code shaped to pass particular tests. If the task looks infeasible, contradicts itself, or a test seems incorrect, stop and tell the user what you found instead of working around it. Reporting a problem is a good outcome here.
