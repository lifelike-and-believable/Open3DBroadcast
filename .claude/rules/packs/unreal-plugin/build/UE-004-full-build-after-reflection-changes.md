---
id: UE-004
title: Do a full build after header or reflection changes
level: SHOULD
scope: pack:unreal-plugin
paths: ["**/*.h", "**/*.Build.cs", "**/*.uplugin", "**/*.Target.cs"]
verified-by: [review]
targets-failure: unverified-claim
observed-on: []
rationale: Live Coding can only patch function bodies; changes to headers, reflection macros, or build files applied through it leave the editor in an inconsistent state.
sources: ["practices/unreal-plugin-development.md", "docs/research/notes/unreal_agentic_development.md"]
---
After changing a header, a `UPROPERTY`/`UFUNCTION`/`UCLASS`/`USTRUCT` declaration, a `.Build.cs`, a `.Target.cs`, or the `.uplugin`, close the editor and run the full build command from AGENTS.md. Use Live Coding only for edits inside `.cpp` function bodies. Report which kind of build you ran.
