---
id: TEST-004
title: Develop new behaviour test first
level: SHOULD
scope: core
verified-by: [review]
targets-failure: unverified-claim
observed-on: [claude-sonnet-5-5]
rationale: The project owner works test-driven where it makes sense; a test written first pins down the intended behaviour, and in the seed evals the test-first rule was the clearest behaviour change the rules produced.
sources: ["practices/testing-and-code-quality.md", "evals/results/2026-10-05-seed/report.md"]
---
Work test-driven when adding or changing behaviour that a test can express: write a small failing test for the next piece of behaviour, run it and confirm it fails for the expected reason, write the least code that makes it pass, then refactor with the tests green. Repeat in small steps. Include the red and green test runs in your report.

Skip test-first for exploratory spikes, purely visual or layout changes, and configuration; say so in your report when you do.
