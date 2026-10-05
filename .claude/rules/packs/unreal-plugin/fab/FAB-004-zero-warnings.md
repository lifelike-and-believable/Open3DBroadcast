---
id: FAB-004
title: Build without warnings on every supported version
level: MUST
scope: pack:unreal-plugin
paths: ["**/*.h", "**/*.cpp", "**/*.Build.cs"]
verified-by: [ci]
check: "ci: Tier 2 runs RunUAT BuildPlugin per engine version and fails on any warning"
targets-failure: convention-drift
observed-on: []
rationale: Fab requires no errors or "consequential warnings" and does not define the term, so zero warnings is the safe bar (4.3.6.2.a).
sources: ["practices/unreal-plugin-development.md", "docs/research/sources/fab-requirements.md"]
---
Fix compiler and UnrealBuildTool warnings you introduce rather than suppressing them. Do not disable warnings with pragmas or build settings unless the user agrees and the reason is recorded next to the suppression.
