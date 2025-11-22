# Implementation Plan Template

Use this structure when creating comprehensive implementation plans.

## Template

```markdown
# Implementation Plan: [Feature/Fix Name]

## Executive Summary
[2-3 sentences: what, why, and expected outcome]

## Requirements
### Functional Requirements
- REQ-1: [Description]
- REQ-2: [Description]

### Non-Functional Requirements  
- NFREQ-1: [Performance/reliability/compatibility requirement]

## Architecture
[Brief description of how components interact, possibly a diagram]

### Key Components
- **ComponentA**: [Responsibility]
- **ComponentB**: [Responsibility]

## Implementation Tasks

### Phase 1: Foundation [Can start immediately]
#### Task 1.1: [Name]
[Use task template from task-template.md]

#### Task 1.2: [Name]  
[Use task template from task-template.md]

### Phase 2: Integration [Depends on Phase 1]
#### Task 2.1: [Name]
[Use task template from task-template.md]

### Phase 3: Testing & Documentation [Depends on Phase 2]
#### Task 3.1: [Name]
[Use task template from task-template.md]

## Testing Strategy
- **Unit Tests**: [What to test]
- **Integration Tests**: [What to test]
- **Manual Testing**: [How to validate]

## Documentation Updates
- [ ] Update README.md
- [ ] Update API documentation
- [ ] Update CHANGELOG.md

## Risks & Mitigation
- **Risk 1**: [Description] → **Mitigation**: [Strategy]

## Timeline
- Phase 1: [Estimate]
- Phase 2: [Estimate]  
- Phase 3: [Estimate]

## Success Criteria
- [ ] All functional requirements met
- [ ] All tests passing
- [ ] Documentation updated
- [ ] Code reviewed and merged
```

## Plan Quality Standards

All plans must:
- **Be specific and actionable** - no vague descriptions
- **Include verification steps** - how to prove it works
- **Reference authoritative sources** - links to docs, APIs, prior art
- **Consider edge cases** - error handling, boundary conditions
- **Respect project constraints** - no blocking on game thread, schema versioning rules, etc.
- **Align with repository guidelines** - follow `.github/copilot-instructions.md`
