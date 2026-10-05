---
id: TEST-003
title: Test behaviour through public interfaces
level: SHOULD
scope: core
paths: ["**/*.test.*", "**/*.spec.*", "**/test/**", "**/tests/**", "**/Tests/**", "**/__tests__/**"]
verified-by: [review]
targets-failure: test-gaming
observed-on: []
rationale: Tests coupled to internals or padded with mocks pass while behaviour is broken, and they break on harmless refactors.
sources: ["practices/testing-and-code-quality.md"]
---
Assert observable behaviour through the code's public interface. Mock only what crosses a process or network boundary, never the unit under test. Wait for conditions rather than fixed delays, because sleeps make tests slow and flaky. Match the style of the nearest existing tests.
