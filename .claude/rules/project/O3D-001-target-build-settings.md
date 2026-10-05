---
id: O3D-001
title: Target.cs build settings for UE 5.7
level: SHOULD
scope: project
paths: ["**/*.Target.cs"]
verified-by: [review]
targets-failure: project-decision
observed-on: []
rationale: The project supports only UE 5.7, so the include order is pinned to it rather than Latest; replaces UE-006.
---
In every `*.Target.cs`, set `DefaultBuildSettings = BuildSettingsVersion.V6;` and `IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;`, because the project builds with Unreal Engine 5.7 only. Build with the UE 5.7 command in AGENTS.md.
