---
id: TEST-002
title: Reproduce a bug with a failing test before fixing it
level: SHOULD
scope: core
verified-by: [review]
targets-failure: unverified-claim
observed-on: []
rationale: A test that fails before the fix and passes after it is direct evidence the bug is fixed, and agents otherwise claim fixes they have not demonstrated.
sources: ["practices/testing-and-code-quality.md", "docs/research/notes/context_and_workflows.md"]
---
When fixing a bug, first add a test that reproduces it and confirm the test fails. Then fix the code and show the same test passing. If the bug cannot be reproduced in a test, say why in your report.
