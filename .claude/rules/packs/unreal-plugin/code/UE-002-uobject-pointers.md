---
id: UE-002
title: Hold UObjects in reflected pointers
level: MUST
scope: pack:unreal-plugin
paths: ["**/*.h", "**/*.cpp", "**/*.inl"]
verified-by: [ci]
check: "ci: Tier 1 unreal check flags TObjectPtr members without UPROPERTY"
targets-failure: invented-api
observed-on: []
rationale: The garbage collector only tracks UObject references it can see through reflection; a missing UPROPERTY causes dangling pointers, and neither the compiler nor Rider reports it.
sources: ["practices/unreal-plugin-development.md", "docs/research/notes/unreal_followup.md"]
---
Declare every UObject member of a UCLASS or USTRUCT as `UPROPERTY() TObjectPtr<T>`. Use raw pointers only for function parameters and locals. Outside UObjects, hold references with `TStrongObjectPtr` (owning) or `TWeakObjectPtr` (non-owning), and check weak pointers with `IsValid()` before use.
