---
id: UE-008
title: Design the Blueprint API for the people who use it
level: SHOULD
scope: pack:unreal-plugin
paths: ["**/*.h"]
verified-by: [review]
targets-failure: project-decision
observed-on: []
rationale: A plugin's users are designers and developers working in the editor, so its Blueprint surface is its user interface.
sources: ["practices/unreal-plugin-development.md"]
---
Expose to Blueprints only what users need. Give every exposed `UFUNCTION` and `UPROPERTY` a `Category` and a tooltip comment, and use display names that read well in the node picker. Mark functions `BlueprintPure` only when they have no side effects. Prefer a `UGameInstanceSubsystem`, `UWorldSubsystem`, or `UEngineSubsystem` as the plugin's entry point over singletons or actors placed in the level.
