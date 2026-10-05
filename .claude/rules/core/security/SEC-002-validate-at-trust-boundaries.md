---
id: SEC-002
title: Validate untrusted input at trust boundaries
level: SHOULD
scope: core
verified-by: [review]
targets-failure: security-regression
observed-on: []
rationale: Generated code frequently contains injection and validation flaws, and newer models were not measurably more secure in published testing.
sources: ["practices/security-for-agentic-projects.md", "docs/research/findings.md"]
---
Treat data from users, networks, files, environment variables, and replicated clients as untrusted. Validate it against an explicit schema or type where it enters the system, use parameterized queries and the framework's escaping, and check authorization on the server for every protected action.
