---
id: WA-006
title: Check unfamiliar APIs against installed sources
level: SHOULD
scope: core
verified-by: [review]
targets-failure: invented-api
observed-on: []
rationale: Incorrect API usage is the largest category of LLM code errors in published studies, and framework and engine APIs change between versions faster than training data.
sources: ["practices/working-with-agents.md", "docs/research/findings.md"]
---
Before using a library, framework, or engine API you have not already seen in this repository, confirm it exists in the installed version: read its type definitions, headers, or bundled docs, or run the tool's `--help`. Treat compiler and type-checker output as authoritative over your memory of an API.
