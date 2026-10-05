---
id: CODE-002
title: Write only the code the task needs
level: SHOULD
scope: core
verified-by: [review]
targets-failure: over-engineering
observed-on: [claude-opus-4-5, claude-opus-4-6]
rationale: Over-engineering is a documented tendency of current models, and Anthropic publishes counter-wording for it.
sources: ["practices/testing-and-code-quality.md", "docs/research/notes/instruction_design.md"]
---
Keep solutions as simple as the task allows. Do not add abstractions with a single implementation, configuration options nobody asked for, or error handling for situations that cannot occur. Handle errors where failure is possible: input from users, files, networks, and other processes. Delete code your change made unused instead of commenting it out.
