# Issue M0.3 - Transform Space, Coordinate System, and Timing Decisions

> **Milestone**: M0 - Protocol and Role Alignment  
> **Area**: protocol, transforms, timing, documentation  
> **Priority**: Critical (affects all transform data)  
> **Estimated Effort**: 2-3 days  
> **Dependencies**: M0.1 (Protocol Message Types and Versioning Confirmation)

## Context and Purpose

Open3DBroadcast will capture and stream skeletal bone transforms from Unreal Engine to remote receivers (LiveLink Source in other Unreal instances, Maya, MotionBuilder). For this to work correctly, we must make explicit decisions about:

1. **Transform Space**: Component-space, bone-space (parent-relative), or world-space?
2. **Coordinate System**: Unreal's left-handed Z-up system; does protocol require conversion?
3. **Units**: Unreal uses centimeters; do receivers expect the same?
4. **Timing**: How to timestamp frames for synchronization and LiveLink alignment?

These decisions must be documented clearly so that all broadcast implementations are consistent and receivers can correctly interpret the data.

## Tasks

### 1. Review Protocol Transform Representation
- [ ] Study transform fields in [`src/o3ds.fbs`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds.fbs):
  - Position (X, Y, Z)
  - Rotation (quaternion or Euler angles?)
  - Scale (uniform or per-axis?)
- [ ] Document existing protocol conventions:
  - Is there a documented coordinate system expectation?
  - Are units specified in schema comments?
  - Is transform space indicated per-message or implicit?
- [ ] Check if protocol supports metadata for coordinate system/units

### 2. Review Existing Receiver Implementations
- [ ] Study Unreal LiveLink Source (`plugins/unreal/Open3DStream/Source/Open3DStream/Private/`):
  - How does it interpret incoming transforms?
  - Does it assume component-space or bone-space?
  - Does it perform any coordinate conversions?
  - What coordinate system does it expect?
- [ ] Check Maya plugin (`plugins/maya/`):
  - Maya uses right-handed Y-up; is conversion happening?
  - What transform space does Maya receiver expect?
- [ ] Check MotionBuilder plugin (`plugins/mobu/`) if available
- [ ] Document any inconsistencies or ambiguities found

### 3. Decide Transform Space for Broadcast
- [ ] Evaluate options:
  - **Component-space (relative to skeletal mesh component root)**:
    - Pros: Consistent across different world positions, simpler for retargeting
    - Cons: Requires component-relative extraction
  - **Bone-space (parent-relative)**:
    - Pros: Matches typical animation data format, compact
    - Cons: Requires bone hierarchy knowledge at receiver
  - **World-space (absolute)**:
    - Pros: Simple to extract, no hierarchy needed
    - Cons: Large values, breaks when component moves
- [ ] **DECISION**: Document chosen transform space with rationale
- [ ] Document how to extract transforms in chosen space from `USkeletalMeshComponent`:
  - For component-space: use `GetComponentSpaceTransforms()`
  - For bone-space: compute parent-relative from component-space
  - For world-space: use `GetSocketTransform()` or component-to-world transforms
- [ ] Verify chosen space matches what existing receivers expect

### 4. Decide Coordinate System and Units
- [ ] **Coordinate System**:
  - Unreal uses left-handed Z-up coordinate system
  - **DECISION**: Does broadcast send Unreal coordinates as-is, or convert to right-handed Y-up?
  - Document conversion formulas if needed (axis swizzling, handedness flip)
  - Note: Receivers may handle conversion themselves; check receiver expectations
- [ ] **Units**:
  - Unreal uses centimeters for distance
  - **DECISION**: Send centimeters as-is, or convert to meters/other?
  - Document unit multiplier if conversion is needed
  - Verify receiver unit expectations
- [ ] **Rotation Representation**:
  - Unreal uses FQuat (quaternion); does protocol use quaternions or Euler angles?
  - Document rotation order if Euler (e.g., XYZ, ZYX)
  - Document quaternion convention (W, X, Y, Z) vs (X, Y, Z, W)
- [ ] Create reference table:
  
  | Aspect | Unreal Native | Broadcast Sends | Receiver Expects | Conversion Needed? |
  |--------|---------------|-----------------|------------------|--------------------|
  | Coord System | Left-handed Z-up | (TBD) | (TBD) | (TBD) |
  | Units | Centimeters | (TBD) | (TBD) | (TBD) |
  | Rotation | FQuat (W,X,Y,Z) | (TBD) | (TBD) | (TBD) |
  | Transform Space | Multiple available | (TBD) | (TBD) | (TBD) |

### 5. Decide Timing and Synchronization
- [ ] **Timecode Source**:
  - Unreal provides `FQualifiedFrameTime` (timecode + subframe)
  - Options: Use Unreal's timecode system, engine time, system clock
  - **DECISION**: Document chosen time source
- [ ] **Frame Number**:
  - LiveLink uses frame numbers for sequencing
  - **DECISION**: How to compute frame number from time source?
  - Should frame numbers be monotonic across sessions or reset?
- [ ] **Timestamp Precision**:
  - Protocol may support multiple timestamp fields
  - **DECISION**: Include both timecode (for editorial sync) and monotonic time (for jitter analysis)?
  - Document timestamp units (seconds, milliseconds, ticks)
- [ ] **Synchronization Strategy**:
  - How to handle time drift between sender and receiver?
  - Should timestamps be relative to session start or absolute?
  - Document expected receiver behavior (interpolation, buffering)
- [ ] Create timing reference table:

  | Timing Aspect | Source | Format | Precision | Notes |
  |---------------|--------|--------|-----------|-------|
  | Timecode | (TBD) | (TBD) | (TBD) | For editorial sync |
  | Frame Number | (TBD) | uint64 | 1 frame | For sequencing |
  | Monotonic Time | (TBD) | (TBD) | (TBD) | For latency/jitter |

### 6. Create Transform and Timing Reference Document
- [ ] Create comprehensive reference (e.g., `docs/BROADCAST_TRANSFORM_TIMING.md`)
- [ ] Include:
  - Transform space decision with rationale
  - Coordinate system and unit specifications
  - Rotation representation details
  - Code snippets for extracting transforms in chosen space/format from Unreal
  - Timing/timecode extraction code snippets
  - Conversion formulas (if any)
  - Validation approach to verify correct transform capture
- [ ] Provide examples:
  - Sample bone transform values (position, rotation, scale)
  - Sample timecode/frame number computation
  - Expected receiver interpretation
- [ ] Document troubleshooting:
  - How to verify transforms are correct (visual comparison)
  - Common mistakes (wrong space, flipped axes, wrong units)
  - Debug visualization approach

### 7. Validate Decisions with Test Data
- [ ] Create or identify test skeletal mesh in Unreal
- [ ] Capture sample transforms in chosen space/format
- [ ] Verify transforms visually match expected animation:
  - Log transform values
  - Compare with Anim Previewer or AnimBP debugger
  - Check if bone orientations are correct
- [ ] If possible, send test message to existing LiveLink receiver and verify display
- [ ] Document validation results

## Acceptance Criteria

- [ ] **Transform Space Decided**: Explicit decision documented with rationale
- [ ] **Coordinate System Defined**: Clear specification of coordinate system and any conversions
- [ ] **Units Specified**: Distance and rotation units clearly documented
- [ ] **Timing Strategy Defined**: Timecode source, frame number computation, and timestamp format specified
- [ ] **Reference Document Created**: Comprehensive guide exists for transform and timing capture
- [ ] **Extraction Code Examples**: Working snippets show how to extract data from Unreal
- [ ] **Validation Complete**: Test data confirms transforms are correct in chosen space/format
- [ ] **Receiver Compatibility**: Decisions align with existing receiver expectations (or conversion plan exists)

## Out of Scope

- Implementing transform capture code (that's M1)
- Performance optimization of transform extraction (that's M5)
- Compression or quantization of transform data (that's M5)
- Retargeting or IK solving (future feature)
- Time synchronization protocols (NTP, PTP) - use simple timestamps for now

## References

### Protocol
- [`src/o3ds.fbs`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds.fbs) - Transform field definitions
- Depends on M0.1 for protocol structure understanding

### Unreal API
- `USkeletalMeshComponent::GetComponentSpaceTransforms()` - Component-space bone transforms
- `USkeletalMeshComponent::GetSocketTransform()` - World-space bone transforms
- `FQualifiedFrameTime` - Unreal timecode system
- `FPlatformTime::Seconds()` - Monotonic time

### Existing Receivers
- Unreal LiveLink Source: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/`
- Maya plugin: `plugins/maya/`
- MotionBuilder plugin: `plugins/mobu/`

### Coordinate System References
- Unreal coordinate system documentation
- Maya coordinate system (right-handed Y-up)
- MotionBuilder coordinate system

### Planning Context
- [`Open3DBroadcast_Planning.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md) (M0 section, "Transform space/coords" item)

## Success Metrics

1. **Clarity**: No ambiguity about transform space, coordinate system, or units
2. **Consistency**: Decisions align across all documentation
3. **Validation**: Test data confirms correct transform capture
4. **Compatibility**: Existing receivers can correctly interpret broadcast data (or conversion is documented)

## Risks and Considerations

- **Receiver Diversity**: Maya, MotionBuilder, and Unreal use different coordinate systems; may need receiver-specific conversion or receiver-side handling
- **Transform Space Complexity**: Choosing wrong space could require rework in M1+; validate decision carefully
- **Timecode Complexity**: Unreal's timecode system can be complex; start simple and iterate if needed
- **Bone Hierarchy**: Parent-relative transforms require consistent bone ordering; document hierarchy handling
- **Floating Point Precision**: Large world-space coordinates may lose precision; consider if this affects chosen space

## Decision Matrix (to be filled during implementation)

| Decision Point | Options Considered | Chosen Option | Rationale |
|----------------|-------------------|---------------|-----------|
| Transform Space | Component / Bone / World | (TBD) | (TBD) |
| Coordinate System | As-is / Convert to RH Y-up | (TBD) | (TBD) |
| Distance Units | Centimeters / Meters | (TBD) | (TBD) |
| Rotation Format | Quaternion / Euler | (TBD) | (TBD) |
| Timecode Source | QualifiedFrameTime / EngineTime / SystemTime | (TBD) | (TBD) |
| Frame Number | Monotonic / Session-relative / Timecode-derived | (TBD) | (TBD) |

## Next Steps After Completion

Once this issue is complete, developers can proceed with:
- **M0.4**: Transport role pairing documentation
- **M1**: Single-mesh pose capture implementation (will use transform/timing specs)
- **M5**: Rate control and quantization (may revisit precision decisions)

---

**Labels**: `milestone:M0`, `area:transforms`, `area:timing`, `area:protocol`, `area:documentation`, `critical`  
**Assignee**: (TBD - requires strong understanding of Unreal animation system and coordinate systems)
