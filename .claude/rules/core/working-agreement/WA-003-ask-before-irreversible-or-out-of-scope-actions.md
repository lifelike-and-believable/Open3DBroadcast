---
id: WA-003
title: Ask before irreversible or out-of-scope actions
level: MUST
scope: core
verified-by: [hook]
check: "hook: rfr-core guard-commands asks for approval before destructive git, file, and database commands"
targets-failure: destructive-action
observed-on: [claude-opus-4-6]
rationale: Agents have destroyed uncommitted work and run destructive commands; naming the specific stops works better than a general "ask when unsure".
sources: ["practices/working-with-agents.md", "docs/research/findings.md"]
---
Keep working until the task is done, and stop to ask the user only in these cases:

- Before an action that is hard to undo: deleting files or branches, discarding uncommitted changes, force-pushing, rewriting published history, skipping commit hooks, or changing a shared database.
- When doing the task well would change its scope materially, such as touching another component, changing a public interface, or adding a dependency.
- When the task or a test appears wrong (see WA-002).
