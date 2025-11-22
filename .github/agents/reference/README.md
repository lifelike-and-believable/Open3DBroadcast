# Planning Agent Reference Documentation

This directory contains detailed reference documentation for the Planning Agent. These documents are extracted from the main agent descriptor to reduce token count while maintaining all essential guidance.

## Document Overview

### [task-template.md](task-template.md)
Comprehensive task structure template with guidelines for:
- Task decomposition and granularity
- Dependency analysis (sequential vs. parallel tasks)
- Task ordering principles
- Complete template format

**Use when:** Breaking down work into individual tasks

### [plan-template.md](plan-template.md)
Complete implementation plan template including:
- Executive summary structure
- Requirements sections (functional and non-functional)
- Architecture documentation
- Implementation tasks organization
- Testing strategy
- Quality standards

**Use when:** Creating comprehensive implementation plans

### [handoff-guide.md](handoff-guide.md)
Specifications for effective handoffs to coding agents:
- Context package contents
- Clear instruction format
- Technical specifications
- Validation requirements
- Communication protocols

**Use when:** Assigning tasks to coding agents

### [research-checklist.md](research-checklist.md)
Detailed research checklist covering:
- Codebase analysis procedures
- API and documentation research
- Testing infrastructure identification
- CI/CD and build system review

**Use when:** Conducting research before creating a plan

## Usage Pattern

The main planning-agent.md file references these documents for detailed guidance. Agents should:

1. Start with the main planning-agent.md for workflow and responsibilities
2. Reference specific documents as needed during the planning process
3. Follow templates when creating tasks and plans
4. Use checklists to ensure thorough research

## Benefits of This Structure

- **Reduced token count**: Main descriptor is ~55% smaller
- **On-demand loading**: Reference docs loaded only when needed
- **Better organization**: Related content grouped logically
- **Easier maintenance**: Update templates without changing main descriptor
- **Reusability**: Templates can be referenced by other agents if needed
