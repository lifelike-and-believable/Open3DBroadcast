# Task Structure Template

Use this template when breaking down work into individual tasks.

## Template

```markdown
### Task: [Short descriptive name]

**Type:** [Implementation / Testing / Documentation / Research]
**Dependencies:** [None / List tasks that must complete first]
**Can run in parallel with:** [Task IDs or "None"]

**Objective:**
[1-2 sentences describing what needs to be done]

**Success Criteria:**
- [ ] Specific, testable criterion 1
- [ ] Specific, testable criterion 2

**Files to modify:**
- `path/to/file1.cpp` - [what changes]
- `path/to/file2.h` - [what changes]

**Testing requirements:**
- What tests to add/modify
- How to verify the change works

**Resources:**
- Link to relevant API documentation
- Link to related issues/PRs
- Link to design documents

**Estimated effort:** [1-2 hours / 2-4 hours / etc.]

**Handoff notes for coding agent:**
- Any specific implementation guidance
- Edge cases to handle
- Code patterns to follow
```

## Guidelines

### Task Granularity
- **Each task should be completable in 1-4 hours** of focused work
- **Each task should have a clear definition of done** with testable outcomes
- **Tasks should minimize risk** - smaller changes are easier to review and debug
- **Tasks should align with PR best practices** - one focused change per PR

### Dependency Analysis
- **Sequential tasks** (must be done in order):
  - Clearly state the dependency: "Task B depends on Task A completion"
  - Provide rationale: why does the order matter
  - Note what outputs from Task A are needed for Task B
  
- **Parallel tasks** (can be done simultaneously):
  - Explicitly mark as "Can be done in parallel with Task X"
  - Ensure tasks don't modify the same files or systems
  - Consider potential merge conflicts and coordinate accordingly

### Task Ordering Principles
1. **Foundation first**: Core data structures, interfaces, base classes
2. **Implementation next**: Concrete implementations that use the foundation
3. **Integration then**: Connecting components together
4. **Testing throughout**: Unit tests alongside implementation, integration tests after
5. **Documentation last**: Update docs after functionality is proven
