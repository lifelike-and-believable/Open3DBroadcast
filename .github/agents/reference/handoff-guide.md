# Handoff Specifications Guide

This guide details how to create effective handoff specifications when assigning tasks to coding agents.

## Context Package
Provide complete context for the coding agent:

- **Link to this plan** so agents can see the big picture
- **Link to requirements** in the original issue
- **Summary of what's been completed** to orient the agent
- **Architecture diagrams or explanations** if the change is complex

## Clear Instructions
Define scope and approach:

- **What to build** - specific functionality required
- **How to build it** - architectural guidance, patterns to follow
- **What NOT to change** - scope boundaries, existing code to preserve
- **Definition of done** - specific, testable acceptance criteria

## Technical Specifications
Provide detailed technical guidance:

- **API signatures** - exact method signatures if known
- **Data structures** - schemas, class hierarchies
- **Error handling** - expected error cases and how to handle them
- **Performance requirements** - latency, throughput, memory constraints
- **Thread safety** - which thread(s) will call this code, synchronization needs

## Validation Requirements

### Testing Checklist
- [ ] Unit tests for new functions/classes
- [ ] Integration tests for component interactions
- [ ] Automation tests for Unreal functionality
- [ ] Manual testing steps if applicable

### Build Verification
- [ ] Code compiles on [Windows / Linux / MacOS]
- [ ] No new compiler warnings
- [ ] Existing tests still pass

### Code Quality Checks
- [ ] Follows repository coding standards
- [ ] No security vulnerabilities introduced
- [ ] Documentation updated

## Communication Protocol
Establish clear communication expectations:

- **How to report progress**: Use the `report_progress` tool after each meaningful milestone
- **How to ask questions**: Comment on the task issue with specific questions
- **When to escalate**: If blocked for more than X hours, if requirements are unclear
- **How to indicate completion**: Final PR linked to task issue, all acceptance criteria met
