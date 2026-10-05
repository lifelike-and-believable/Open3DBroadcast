---
id: CODE-004
title: Add dependencies deliberately
level: SHOULD
scope: core
verified-by: [review]
targets-failure: invented-api
observed-on: [gpt-4, gpt-4-turbo]
rationale: About a fifth of package names suggested by models in one large study did not exist, and recurring invented names can be registered by attackers.
sources: ["practices/testing-and-code-quality.md", "docs/research/notes/verification_of_secondary_figures.md"]
---
Prefer the standard library and dependencies the project already has. Before adding a new package, confirm its exact name exists in the registry, that it is maintained, and that its licence suits the project. Use the project's package manager so the lockfile is updated, and list each new dependency and why in your report.
