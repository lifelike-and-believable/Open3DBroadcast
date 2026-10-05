---
id: CODE-003
title: Comments explain why
level: SHOULD
scope: core
verified-by: [review]
targets-failure: convention-drift
observed-on: [claude-fable-5]
rationale: Narrating comments add noise and go stale; Anthropic lists them among the elaborations current models add unprompted.
sources: ["practices/testing-and-code-quality.md", "docs/research/notes/instruction_design.md"]
---
Add a comment only where the reason for the code is not evident from the code itself: a constraint, a workaround, or a non-obvious decision. Do not narrate what the next line does, and do not add comments, docstrings, or type annotations to code you did not change.
