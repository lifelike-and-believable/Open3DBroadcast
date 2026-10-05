---
id: CODE-001
title: Follow the repository's existing patterns
level: SHOULD
scope: core
verified-by: [review]
targets-failure: convention-drift
observed-on: []
rationale: Pointing at an existing example is cheaper and more accurate than describing a pattern, and agent code otherwise drifts from local conventions.
sources: ["practices/testing-and-code-quality.md", "docs/research/findings.md"]
---
Before writing new code, find the most similar existing file in this repository and follow its structure, naming, error handling, and test layout. Name that file in your report. Reuse existing helpers instead of writing near-duplicates; search for one before adding a new function.
