---
id: UE-003
title: Follow the Epic coding standard where agents usually drift
level: SHOULD
scope: pack:unreal-plugin
paths: ["**/*.h", "**/*.cpp", "**/*.inl"]
verified-by: [review]
targets-failure: convention-drift
observed-on: []
rationale: These are the points of Epic's current coding standard that models trained on general C++ most often get wrong; formatting is left to clang-format.
sources: ["practices/unreal-plugin-development.md", "docs/research/notes/unreal_followup.md"]
---
Write C++20 in Epic style:

- Use `auto` only for lambdas, iterators, and template types that are hard to name. Do not use structured bindings.
- List lambda captures explicitly. Capture weak pointers in lambdas that may run after the owner is destroyed.
- Prefix booleans with `b`, use engine containers (`TArray`, `TMap`, `FString`) and `TEXT()` for string literals.
- Put the module's own header first in each `.cpp` and include only what is used; do not include `CoreMinimal.h` in place of specific headers in new code.
- Do not mark return types `const`.
