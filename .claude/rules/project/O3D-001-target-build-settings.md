---
id: O3D-001
title: Target.cs build settings per engine (UE 5.7 and 5.8)
level: SHOULD
scope: project
paths: ["**/*.Target.cs"]
verified-by: [review]
targets-failure: project-decision
observed-on: []
rationale: The project supports UE 5.7 and 5.8 (ADR 0014), so each engine gets its own settings rather than Latest; replaces UE-006.
---
In every `*.Target.cs`, set `DefaultBuildSettings = BuildSettingsVersion.V7;` and `IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;` under `#if UE_5_8_OR_LATER`, and `BuildSettingsVersion.V6` with `EngineIncludeOrderVersion.Unreal5_7` under `#else`, because the project builds with Unreal Engine 5.7 and 5.8 (ADR 0014). UnrealBuildTool defines `UE_5_<n>_OR_LATER` up to its own minor version. Build with the commands in AGENTS.md.
