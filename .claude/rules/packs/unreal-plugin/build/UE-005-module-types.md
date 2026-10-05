---
id: UE-005
title: Use current module types and keep editor code in editor modules
level: MUST
scope: pack:unreal-plugin
paths: ["**/*.uplugin", "**/*.Build.cs"]
verified-by: [ci]
check: "ci: Tier 1 unreal check rejects the Developer module type"
targets-failure: stale-knowledge
observed-on: []
rationale: The Developer module type is deprecated, yet Epic's own Plugins page still shows it, so models copy it.
sources: ["practices/unreal-plugin-development.md", "docs/research/notes/unreal_followup.md"]
---
Give each module in the `.uplugin` one of `Runtime`, `Editor`, `UncookedOnly`, or `DeveloperTool`; never `Developer`. Put code that uses editor-only modules (`UnrealEd`, `Slate` editor widgets, asset tools) in an `Editor` or `UncookedOnly` module so packaged games never link it. Trust the `FModuleDescriptor` API reference over narrative docs for field names.
