# Open3DBroadcast M0 Issues - Index and Quick Reference

> **Milestone**: M0 - Protocol and Role Alignment  
> **Status**: Issues Created - Ready for Implementation  
> **Created**: October 19, 2025

## Overview

This document provides a quick reference to all M0 milestone issues for the Open3DBroadcast implementation. M0 focuses on establishing the foundational protocol, coordinate system, timing, and transport decisions before any code implementation begins.

## Why M0 Matters

M0 is the critical foundation phase that must be completed before implementing Open3DBroadcast. These decisions affect:
- How transforms and curves are captured and interpreted
- Protocol compatibility with existing receivers
- Transport configuration and connection establishment
- Overall architecture and implementation approach

Getting M0 right prevents costly rework in later milestones (M1-M9).

## M0 Issues Summary

### Issue M0.1 - Protocol Message Types and Versioning Confirmation
**File**: [ISSUE_M0_1_PROTOCOL_ALIGNMENT.md](./ISSUE_M0_1_PROTOCOL_ALIGNMENT.md)

**Purpose**: Confirm and document the exact message types, versioning strategy, and serialization APIs from the existing O3DS protocol.

**Key Tasks**:
- Review FlatBuffers schema (`src/o3ds.fbs`)
- Document protocol message structure for broadcast use case
- Study serialization APIs in `src/o3ds/model.h/.cpp`
- Create protocol reference documentation

**Estimated Effort**: 1-2 days  
**Dependencies**: None (foundation issue)

**Deliverables**:
- Protocol reference document
- Serialization code examples
- Validation against existing receivers

---

### Issue M0.2 - Curve Semantics and Filtering Rules
**File**: [ISSUE_M0_2_CURVE_SEMANTICS.md](./ISSUE_M0_2_CURVE_SEMANTICS.md)

**Purpose**: Define curve naming conventions, value ranges, and filtering rules based on `CURVE_SUPPORT.md`.

**Key Tasks**:
- Review curve support documentation
- Study existing curve tests
- Define Unreal-to-protocol curve mapping
- Specify filtering and normalization rules

**Estimated Effort**: 1-2 days  
**Dependencies**: M0.1

**Deliverables**:
- Curve capture reference document
- Naming convention mapping
- Filtering rules specification

---

### Issue M0.3 - Transform Space, Coordinate System, and Timing Decisions
**File**: [ISSUE_M0_3_TRANSFORM_SPACE.md](./ISSUE_M0_3_TRANSFORM_SPACE.md)

**Purpose**: Make explicit decisions about transform space, coordinate system, units, and timing/timecode strategy.

**Key Tasks**:
- Decide transform space (component-space, bone-space, or world-space)
- Define coordinate system and unit conventions
- Specify timing/timecode source and format
- Validate decisions against existing receivers

**Estimated Effort**: 2-3 days  
**Dependencies**: M0.1  
**Priority**: Critical (affects all transform data)

**Deliverables**:
- Transform and timing reference document
- Decision matrix with rationale
- Extraction code examples
- Validation test results

---

### Issue M0.4 - Transport Role Pairing Documentation
**File**: [ISSUE_M0_4_TRANSPORT_ROLES.md](./ISSUE_M0_4_TRANSPORT_ROLES.md)

**Purpose**: Document the exact role pairing for each transport (TCP, UDP, NNG, WebRTC) to ensure correct broadcast/receiver configuration.

**Key Tasks**:
- Review core transport implementations
- Map LiveLink Source options to broadcast roles
- Create transport pairing matrix
- Document common configuration scenarios

**Estimated Effort**: 2-3 days  
**Dependencies**: M0.1

**Deliverables**:
- Transport pairing matrix
- URL format specifications
- Configuration scenario examples
- Transport selection guide
- Troubleshooting guide

---

## Implementation Order

Recommended sequence for maximum efficiency:

1. **Start with M0.1** (Protocol Alignment) - Foundation for all other issues
2. **Parallel track M0.2 and M0.3** - Can be done simultaneously by different agents
   - M0.2 (Curve Semantics) - Focuses on curves
   - M0.3 (Transform Space) - Focuses on transforms and timing
3. **Parallel track M0.4** (Transport Roles) - Can start after M0.1, independent of M0.2/M0.3
4. **Review and finalize** - Ensure all four issues align and reference each other correctly

```
M0.1 (Protocol Alignment)
  ├─→ M0.2 (Curve Semantics)
  ├─→ M0.3 (Transform Space)
  └─→ M0.4 (Transport Roles)
```

## Acceptance Criteria for M0 Completion

The M0 milestone is complete when:

- [ ] All four issue documents are completed with deliverables
- [ ] Protocol reference documentation exists and is validated
- [ ] Curve naming, ranging, and filtering rules are documented
- [ ] Transform space, coordinate system, and timing decisions are made and documented
- [ ] Transport pairing matrix is complete with examples
- [ ] All documentation cross-references are consistent
- [ ] Decisions have been validated against existing receivers where possible
- [ ] Documentation is stored in appropriate location (e.g., `docs/` directory or root)

## Documentation Location

Suggested structure for M0 deliverables:

```
/home/runner/work/Open3DStream/Open3DStream/
├── docs/                                    (create if doesn't exist)
│   ├── BROADCAST_PROTOCOL_REFERENCE.md      (from M0.1)
│   ├── BROADCAST_CURVE_CAPTURE.md           (from M0.2)
│   ├── BROADCAST_TRANSFORM_TIMING.md        (from M0.3)
│   └── BROADCAST_TRANSPORT_GUIDE.md         (from M0.4)
├── ISSUE_M0_1_PROTOCOL_ALIGNMENT.md         (issue spec)
├── ISSUE_M0_2_CURVE_SEMANTICS.md            (issue spec)
├── ISSUE_M0_3_TRANSFORM_SPACE.md            (issue spec)
└── ISSUE_M0_4_TRANSPORT_ROLES.md            (issue spec)
```

## Next Steps After M0

Once M0 is complete, proceed to:

- **M1**: Single-mesh capture implementation
  - Use protocol reference from M0.1
  - Apply curve rules from M0.2
  - Use transform/timing specs from M0.3
- **M2**: Serialization integration
  - Build on protocol understanding from M0.1
- **M3**: Transport integration
  - Use transport pairing from M0.4

## Key References

### Planning Document
- [Open3DBroadcast_Planning.md](./Open3DBroadcast_Planning.md) - Master plan with all milestones

### Protocol and Schema
- `src/o3ds.fbs` - FlatBuffers schema
- `src/o3ds_generated.h` - Generated header
- `src/o3ds/model.h`, `src/o3ds/model.cpp` - Serialization APIs

### Curve Documentation
- `CURVE_SUPPORT.md` - Curve naming and ranging conventions
- `test_curve_comprehensive.cpp`, `test_curves.cpp` - Curve tests

### Transport Documentation
- `src/o3ds/tcp.cpp`, `src/o3ds/udp.cpp` - TCP/UDP implementations
- `src/o3ds/nng_connector.h` - NNG connector
- `sphinx/connectors.rst` - Connector overview

### WebRTC (Optional/Beta)
- `WEBRTC_IMPLEMENTATION_SUMMARY.md`
- `WEBRTC_SUPPORT.md`
- `LIBDATACHANNEL_INTEGRATION.md`

## Notes for Coding Agents

When implementing M0 issues:

1. **Focus on documentation, not code** - M0 is about decisions and specifications
2. **Validate against existing code** - Reference implementations exist for receivers
3. **Be explicit and unambiguous** - Avoid "could" or "might"; make clear decisions
4. **Provide examples** - Code snippets, sample values, configuration examples
5. **Cross-reference** - Link between issues and to existing documentation
6. **Consider receiver compatibility** - Decisions must work with existing LiveLink Source, Maya, MotionBuilder
7. **Document rationale** - Explain WHY decisions were made, not just what they are

## Questions or Issues?

If you encounter ambiguities or conflicts while implementing M0 issues:

1. Check the master planning document: `Open3DBroadcast_Planning.md`
2. Reference the Copilot instructions: `.github/copilot-instructions.md`
3. Look at existing receiver implementations for context
4. Document uncertainties and propose solutions in your implementation

## Change Log

- **v1.0** (October 19, 2025): Initial index created with all four M0 issues defined

---

**This index is part of the Open3DStream Open3DBroadcast implementation planning.**  
**For the complete implementation plan, see [Open3DBroadcast_Planning.md](./Open3DBroadcast_Planning.md)**
