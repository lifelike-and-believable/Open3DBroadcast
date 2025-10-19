# Issue M0.1 - Protocol Message Types and Versioning Confirmation

> **Milestone**: M0 - Protocol and Role Alignment  
> **Area**: protocol, documentation  
> **Priority**: High (foundation for all broadcast work)  
> **Estimated Effort**: 1-2 days  
> **Dependencies**: None

## Context and Purpose

Before implementing Open3DBroadcast, we must confirm the exact message types, versioning strategy, and serialization APIs from the existing O3DS protocol. This ensures the broadcast module uses the protocol correctly and maintains compatibility with existing receivers (Unreal LiveLink Source, Maya, MotionBuilder).

The Open3DStream protocol is defined in `src/o3ds.fbs` (FlatBuffers schema) and implemented via model/serializer APIs in `src/o3ds/`. Broadcast must reuse these without modification.

## Tasks

### 1. Review Protocol Schema
- [ ] Read and document the current FlatBuffers schema in `src/o3ds.fbs`
- [ ] Identify message types relevant to broadcast:
  - `Subject` structure (skeleton description + frame data)
  - `SubjectList` envelope
  - Transform representation (position, rotation, scale)
  - Curve data structures (morph targets, named animation curves)
  - Timestamp/timecode fields
- [ ] Note any optional fields and their default values
- [ ] Document protocol version tag (`O3DS_VERSION_TAG`) and compatibility strategy

### 2. Review Model APIs
- [ ] Examine serialization functions in `src/o3ds/model.h` and `src/o3ds/model.cpp`
- [ ] Document how to pack data into FlatBuffers:
  - Creating a `Subject` with skeleton description (bone names, parent indices)
  - Adding per-frame transforms and curves to a `Subject`
  - Wrapping multiple subjects in a `SubjectList`
  - Serializing to binary buffer for transport
- [ ] Document how receivers deserialize and consume messages
- [ ] Identify any existing validation or sanity checks

### 3. Review Existing Usage Examples
- [ ] Study how the Unreal LiveLink Source (receiver) parses messages in `plugins/unreal/Open3DStream/Source/Open3DStream/`
- [ ] Review C++ examples showing protocol usage:
  - Check `README.md` for sender/receiver examples
  - Look at test files like `test_curve_comprehensive.cpp` and `test_curves.cpp`
  - Examine Maya/MotionBuilder plugin sender code if applicable
- [ ] Document the expected message flow for a typical session:
  1. Initial skeleton description send
  2. Per-frame transform + curve updates
  3. Handling skeleton changes (re-send description)

### 4. Create Protocol Reference Document
- [ ] Create a concise reference document (e.g., `docs/BROADCAST_PROTOCOL_REFERENCE.md` or add to existing docs)
- [ ] Include:
  - Message structure diagrams
  - Field-by-field descriptions for broadcast use case
  - Serialization code snippets/examples
  - Version compatibility notes
  - Links to relevant source files
- [ ] Document any protocol limitations or constraints relevant to broadcast:
  - Maximum bone count
  - Maximum curve count
  - Maximum message size considerations
  - Frame ordering requirements (if any)

### 5. Validate Against Existing Receivers
- [ ] Build and run existing C++ examples to observe protocol in action
- [ ] Verify Unreal LiveLink Source can receive test messages
- [ ] Document expected message format with actual binary/hex examples if helpful

## Acceptance Criteria

- [ ] **Protocol Reference Document Created**: Clear documentation of message types, fields, and serialization APIs exists in `docs/` or similar location
- [ ] **Versioning Strategy Documented**: Version tag usage and compatibility rules are clear
- [ ] **Serialization Examples**: Working code snippets show how to create and send messages
- [ ] **No Schema Changes**: Confirmation that broadcast can work with existing schema without modifications
- [ ] **Validation Complete**: Successfully demonstrated that test messages can be created and consumed by existing LiveLink receiver

## Out of Scope

- Modifying `src/o3ds.fbs` schema (only document existing)
- Implementing compression or custom serialization formats
- Adding new message types or fields (unless critical and justified)
- Performance optimization of existing protocol code

## References

### Protocol Definition
- FlatBuffers schema: [`src/o3ds.fbs`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/src/o3ds.fbs)
- Generated header: `src/o3ds_generated.h`
- Model API: `src/o3ds/model.h`, `src/o3ds/model.cpp`

### Documentation
- Main README: [`README.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/README.md)
- Curve support: [`CURVE_SUPPORT.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/CURVE_SUPPORT.md)

### Test Code
- [`test_curve_comprehensive.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curve_comprehensive.cpp)
- [`test_curves.cpp`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/test_curves.cpp)

### Existing Receiver Implementation
- Unreal LiveLink Source: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/`
- Maya plugin: `plugins/maya/`
- MotionBuilder plugin: `plugins/mobu/`

### Planning Context
- [`Open3DBroadcast_Planning.md`](https://github.com/lifelike-and-believable/Open3DStream/blob/develop/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md) (M0 section)

## Success Metrics

1. **Completeness**: All message types and fields relevant to broadcast are documented
2. **Clarity**: A new developer can understand protocol structure from the reference doc alone
3. **Validation**: Reference doc has been reviewed against actual message examples
4. **Reusability**: Document serves as a guide for implementing M1+ tasks

## Risks and Considerations

- **Schema Evolution**: If schema changes are needed, they require regenerating headers with `flatc --cpp src/o3ds.fbs` and updating all consumers
- **Backward Compatibility**: Any future additions must use optional fields to maintain compatibility
- **Platform Differences**: Ensure protocol is platform-agnostic (endianness, struct packing)
- **Size Constraints**: Large skeleton/curve counts may require fragmentation or compression (note but don't implement yet)

## Next Steps After Completion

Once this issue is complete, developers can proceed with:
- **M0.2**: Curve semantics alignment (naming, ranging, filtering)
- **M0.3**: Transform space and coordinate system decisions
- **M0.4**: Transport role pairing documentation
- **M1**: Single-mesh capture implementation (will use this protocol reference)

---

**Labels**: `milestone:M0`, `area:protocol`, `area:documentation`, `good-first-task`, `foundation`  
**Assignee**: (TBD - suitable for coding agent familiar with FlatBuffers and C++)
