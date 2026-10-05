---
id: UE-001
title: Check engine APIs against each supported engine version
level: MUST
scope: pack:unreal-plugin
paths: ["**/*.h", "**/*.cpp", "**/*.inl", "**/*.Build.cs"]
verified-by: [ci]
check: "ci: Tier 2 builds the plugin against every supported engine version"
targets-failure: invented-api
observed-on: []
rationale: Unreal's API is large and changes between releases (5.8 removed many items deprecated in 5.0 to 5.6), and incorrect API use is the largest category of model coding errors.
sources: ["practices/unreal-plugin-development.md", "docs/research/findings.md", "docs/research/notes/unreal_followup.md"]
---
This plugin supports the engine versions listed in AGENTS.md. Before using an engine type, function, macro, or module you have not already seen in this plugin, find its declaration in the installed engine headers for the oldest and newest supported versions. Wrap APIs that exist only in newer versions in `#if` checks on `ENGINE_MAJOR_VERSION` and `ENGINE_MINOR_VERSION`, or a small wrapper in the plugin. Treat compiler errors as the authority over memory.
