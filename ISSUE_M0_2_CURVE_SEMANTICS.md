# Issue M0.2 - Curve Semantics and Filtering Rules

> **Milestone**: M0 - Protocol and Role Alignment  
> **Area**: protocol, curves, documentation  
> **Priority**: High  
> **Estimated Effort**: 1-2 days  
> **Dependencies**: M0.1 (Protocol Message Types and Versioning Confirmation)

## Context and Purpose

Open3DBroadcast needs to capture and stream animation curves (morph targets and named animation curves) from Unreal skeletal meshes. To ensure compatibility with existing receivers and maintain consistency across the ecosystem, we must adopt the curve naming conventions, value ranges, and filtering rules documented in `CURVE_SUPPORT.md`.

This issue defines exactly which curves to capture, how to name them, what value ranges to use, and what filtering/normalization rules to apply before sending via the O3DS protocol.

## Tasks

### 1. Review Curve Support Documentation
- [ ] Read and summarize [`CURVE_SUPPORT.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/CURVE_SUPPORT.md)
- [ ] Document the supported curve types:
  - Morph target curves (blend shapes)
  - Named animation curves (custom curves from AnimBP)
  - Any other curve types supported by the protocol
- [ ] Note the expected naming conventions:
  - Namespace/prefix requirements (if any)
  - Character restrictions
  - Case sensitivity rules
- [ ] Document expected value ranges:
  - Morph targets: typically [0.0, 1.0] or [-1.0, 1.0]?
  - Named curves: unbounded or specific range?
  - How to handle out-of-range values

### 2. Review Existing Curve Tests
- [ ] Study [`test_curve_comprehensive.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curve_comprehensive.cpp)
- [ ] Study [`test_curves.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curves.cpp)
- [ ] Extract examples of:
  - Curve naming patterns used in tests
  - Value ranges validated in tests
  - Serialization/deserialization patterns
  - Edge cases tested (empty curves, max count, special characters in names)
- [ ] Document any filtering logic present in tests

### 3. Define Curve Capture Rules for Unreal
- [ ] Specify which Unreal curve sources to capture from `USkeletalMeshComponent`:
  - Morph target weights from `MorphTargetWeights` array
  - Animation curves from `AnimInstance->GetAnimationCurveList()` or similar
  - Custom curves set via AnimBP or animation assets
- [ ] Define naming mapping from Unreal to O3DS protocol:
  - Morph target name: use as-is or add prefix?
  - Animation curve name: preserve FName or convert?
  - Handle name collisions (if morph and anim curve have same name)
- [ ] Define value normalization/clamping rules:
  - Should morph targets be clamped to [0, 1]?
  - Should animation curves be clamped or passed as-is?
  - How to handle NaN or infinite values (error, clamp, skip)?

### 4. Define Filtering Rules
- [ ] Specify filtering criteria to avoid sending unnecessary curves:
  - Filter curves with zero/default values? (optional optimization)
  - Filter curves not actively changing? (delta threshold)
  - Filter curves based on naming patterns (e.g., exclude internal/system curves)
  - Maximum curve count per subject (if protocol has limits)
- [ ] Consider performance vs. fidelity trade-offs:
  - Sending all curves every frame vs. delta-only updates
  - Bandwidth implications
- [ ] Document filtering configuration:
  - Should filtering be configurable per broadcast component?
  - Default filter settings recommendations

### 5. Create Curve Capture Reference Document
- [ ] Create or extend documentation (e.g., `docs/BROADCAST_CURVE_CAPTURE.md` or add section to protocol reference)
- [ ] Include:
  - Curve types supported by broadcast
  - Unreal-to-protocol naming mapping table
  - Value range specifications with examples
  - Filtering rules and configuration options
  - Code snippets showing how to extract curves from `USkeletalMeshComponent`
  - Troubleshooting guide for common curve issues
- [ ] Provide examples:
  - Morph target "Smile_L" with value 0.75
  - Animation curve "HeartRate" with value 120.0
  - Handling multi-component names or namespaces

### 6. Validate Against Existing Receivers
- [ ] Verify that existing LiveLink Source correctly displays curve data:
  - Test with morph targets
  - Test with named animation curves
  - Verify naming appears correctly in LiveLink UI
- [ ] Check Maya/MotionBuilder receiver behavior if applicable
- [ ] Document any receiver-specific quirks or limitations

## Acceptance Criteria

- [ ] **Curve Capture Reference Created**: Clear documentation exists for curve capture, naming, ranging, and filtering
- [ ] **Naming Convention Defined**: Unambiguous mapping from Unreal curve names to protocol names
- [ ] **Value Range Policy**: Clear rules for normalization/clamping of curve values
- [ ] **Filtering Rules Documented**: Explicit criteria for which curves to send and when
- [ ] **Compatibility Validated**: Reference aligns with `CURVE_SUPPORT.md` and passes review against existing test patterns
- [ ] **Examples Provided**: Working code snippets or pseudocode for curve extraction from Unreal

## Out of Scope

- Implementing curve capture code (that's M1)
- Adding new curve types to protocol schema
- Performance optimization of curve serialization (that's M5)
- Compression or quantization of curve data (that's M5)

## References

### Curve Documentation
- [`CURVE_SUPPORT.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/CURVE_SUPPORT.md)

### Test Code
- [`test_curve_comprehensive.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curve_comprehensive.cpp)
- [`test_curves.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curves.cpp)

### Unreal API References
- `USkeletalMeshComponent::GetMorphTargetWeights()`
- `UAnimInstance::GetAnimationCurveList()` or equivalent
- AnimBP curve access patterns

### Protocol
- Depends on M0.1 for protocol message structure
- [`src/o3ds.fbs`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds.fbs) (curve fields)

### Planning Context
- [`Open3DBroadcast_Planning.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md) (M0 section)

## Success Metrics

1. **Completeness**: All curve types and their handling rules are documented
2. **Clarity**: Developers can implement curve capture without ambiguity
3. **Consistency**: Naming and ranging align with existing CURVE_SUPPORT.md
4. **Testability**: Filtering rules are concrete enough to write unit tests against

## Risks and Considerations

- **Unreal Version Differences**: Curve API may vary between UE 5.3, 5.4, 5.5; document which version APIs are used
- **Morph Target Limits**: Unreal supports many morph targets; consider impact of large morph target counts on bandwidth
- **AnimBP Complexity**: Some curves may be computed per-frame; capture final evaluated values only
- **Name Collisions**: Rare but possible for morph and anim curve to share names; define resolution strategy
- **Performance**: Extracting hundreds of curves per frame may impact performance; note but defer optimization to M5

## Curve Naming Examples (for reference)

| Unreal Source | Example Name | Protocol Name | Value Range | Notes |
|--------------|--------------|---------------|-------------|-------|
| Morph Target | `Smile_L` | `Smile_L` | [0.0, 1.0] | Clamped if outside range |
| Morph Target | `EyeBlink_R` | `EyeBlink_R` | [0.0, 1.0] | Clamped if outside range |
| Anim Curve | `HeartRate` | `HeartRate` | Unbounded | Pass as-is |
| Anim Curve | `LookAtWeight` | `LookAtWeight` | [0.0, 1.0] | Typical range but not enforced |

*(This table should be refined during issue implementation)*

## Next Steps After Completion

Once this issue is complete, developers can proceed with:
- **M0.3**: Transform space and coordinate system decisions
- **M0.4**: Transport role pairing documentation
- **M1**: Single-mesh curve capture implementation (will use these rules)

---

**Labels**: `milestone:M0`, `area:curves`, `area:protocol`, `area:documentation`  
**Assignee**: (TBD - suitable for coding agent familiar with Unreal animation system)
