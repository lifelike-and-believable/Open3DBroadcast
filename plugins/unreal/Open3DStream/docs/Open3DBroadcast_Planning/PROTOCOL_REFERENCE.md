# Open3DStream Protocol Reference (Broadcast)

Purpose
- Establish a concise, implementation-ready reference for using the existing Open3DStream (O3DS) protocol to send skeletal animation poses and curves from Unreal (Open3DBroadcast) to receivers (e.g., Unreal LiveLink source, Maya, MotionBuilder).
- Confirm message types, versioning, and serialization APIs. This is a mapping guide, not a schema redesign.

Scope
- Message coverage: Subject, SubjectUpdate, SubjectList, Transforms, Curves, and the time field.
- Serialization: Use existing FlatBuffers schema in `src/o3ds.fbs` and model helpers in `src/o3ds/model.h/.cpp`.
- Compatibility: Maintain backward compatibility via optional fields and the single source version tag.

Key files to read
- Schema (generated): `src/o3ds_generated.h` (from `src/o3ds.fbs`)
- Model pack/unpack: `src/o3ds/model.h`, `src/o3ds/model.cpp`
- Receiver reference (Unreal): `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`
- Curve tests: `test_curves.cpp`, `test_curve_comprehensive.cpp`

## Data model overview

Messages are batched per frame in a SubjectList envelope:
- Subject: Full description of a skeletal subject (bone transforms, axes, format, curve names+values)
- SubjectUpdate: Per-frame deltas (translations, rotations, scales, curves) aligned by index
- SubjectList: Batches one or more Subjects and/or SubjectUpdates plus a `time` (double, seconds)

Subject
- Fields (from `O3DS::Data::Subject`):
  - nodes: Vector<Transform> — the skeleton’s transform nodes in a stable order
    - Transform: parentId, name, translation, rotation, scale, optional matrices/components
  - name: String — stable subject identifier
  - x_axis, y_axis, z_axis: Direction — orientation metadata
  - format: String — format hint (e.g., coordinate basis or app flavor)
  - curves: Vector<Curve> — curve name/value pairs (optional)

SubjectUpdate
- Fields (from `O3DS::Data::SubjectUpdate`):
  - name: String — subject identifier
  - translations: Vector<TranslationUpdate> (optional)
  - rotation: Vector<RotationUpdate> (optional)
  - scale: Vector<ScaleUpdate> (optional)
  - curves: Vector<CurveUpdate> (optional)
- Used to send deltas without resending the entire skeleton description.

SubjectList
- Fields (from `O3DS::Data::SubjectList`):
  - subjects: Vector<Subject> (optional)
  - updates: Vector<SubjectUpdate> (optional)
  - time: double (seconds) — monotonic timestamp for the batch

Timestamps
- `time` is a double seconds value, typically from a monotonic clock (e.g., `O3DS::GetTime()` in `model.cpp`).
- LiveLink timecode mapping (if needed) should be handled in the engine adapter; protocol currently carries `time` only.

## Versioning and compatibility
- Single source version tag: `O3DS_VERSION_TAG` defined in `CMakeLists.txt` governs protocol package versioning.
- Add new fields as optional with sensible defaults to preserve backward compatibility.
- Regenerate FlatBuffers headers when `src/o3ds.fbs` changes:
  - `flatc --cpp src/o3ds.fbs`
- Bump `O3DS_VERSION_TAG` and update all consumers when schema changes occur.

## Serialization helpers (model API)

Subject serialization (full description)
- `O3DS::Subject::Serialize(flatbuffers::FlatBufferBuilder&)`
  - Packs nodes (transforms), subject name, axes, format, and optional curves.
  - Curves packed via `Subject::SerializeCurves(builder)` creating `O3DS::Data::Curve` entries with name/value.
- Envelope for a single subject:
  - `O3DS::Subject::Serialize(std::vector<char>& outbuf, double timestamp)`
    - Creates a SubjectList with a single Subject and the provided or current timestamp.

Batch serialization (multiple subjects)
- `O3DS::SubjectList::Serialize(std::vector<char>& outbuf, double timestamp)`
  - Iterates `SubjectList::mItems` and packs full Subjects, producing one SubjectList per call.

Update serialization (deltas)
- `O3DS::Subject::SerializeUpdate(flatbuffers::FlatBufferBuilder&, size_t& count, double deltaThreshold)`
  - Produces `O3DS::Data::SubjectUpdate` entries based on thresholds.
- `O3DS::SubjectList::SerializeUpdate(std::vector<char>& outbuf, size_t& count, double timestamp)`
  - Batches multiple SubjectUpdate messages into a SubjectList with `time`.

Parse/unpack
- `O3DS::SubjectList::Parse(const char* data, size_t len, TransformBuilder* builder, bool clearInactive)`
  - Calls `ParseSubject` for Subject entries and `ParseUpdate` for SubjectUpdate entries.
- `ParseSubject` copies subject metadata (axes, format), clears and fills transforms and curves.

## Transform components and `transformOrder` (critical)

- Component-space TRS is the default encoding for bones.
- TRS serialization is controlled by `Transform::transformOrder`:
  - You must set `transformOrder = { TTranslation, TRotation, TScale }` on each `Transform` you intend to serialize.
  - If `transformOrder` is empty, TRS components are not serialized and receivers will reconstruct identity transforms (0 translation, unit scale, identity rotation) unless matrices were sent.
- Do not send both TRS and raw matrices for the same node in a single frame. Prefer TRS to minimize payload size and keep semantics clear.
- Call `Subject::CalcMatrices()` prior to serialization to ensure internal matrices reflect the current TRS (useful for senders/consumers that rely on matrices internally).

Example:
```cpp
auto* t = subj->addTransform("Bone", parentId);
t->translation.value = O3DS::Vector3d(Tx, Ty, Tz);
t->rotation.value    = O3DS::Vector4d(Qx, Qy, Qz, Qw);
t->scale.value       = O3DS::Vector3d(Sx, Sy, Sz);
// Ensure TRS is serialized
t->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };
```

## Field alignment and lifecycle rules
- Bone transforms order: Per Subject, the receiver expects per-frame arrays to align with the Subject’s nodes vector.
- Curves alignment: Curve values are aligned by index to the names vector; send names in Subject; per-frame send values (or updates) only.
- Descriptor resend: If the skeleton topology or curve set/order changes, resend a full Subject before sending further updates (reset delta state).
- Multiple meshes: Batch multiple Subjects (and/or updates) into a single SubjectList per frame for efficiency.

## Coordinate system and units
- Coordinate system: Unreal default (left-handed, Z-up). Carry axes in Subject (x_axis, y_axis, z_axis) for explicitness.
- Units: Unreal centimeters. Do not apply unit scaling in the protocol; handle conversions in adapters if needed.
- Transform space: Use component-space transforms for bones (parent-relative) to match stable skeletal hierarchies.

## Delta/Update frames (later milestones)
- `Subject::SerializeUpdate`/`SubjectList::SerializeUpdate` implement delta-threshold behavior. After successfully transmitting a component, call `translation.sent()`, `rotation.sent()`, and `scale.sent()` to advance last-sent state so `delta()` is meaningful.
- Keep the same `transformOrder` in updates as in the descriptor frame; mismatches will apply deltas in the wrong order.
- If any structural aspect changes (skeleton, curve set, order), send a full Subject frame and reset delta state before resuming updates.

## Minimal C++ packing example (sketch)

```cpp
#include "o3ds/model.h"

std::vector<char> buf;
O3DS::SubjectList list;

// Create/lookup subject
O3DS::Subject* subj = list.addSubject("World/Actor/SkeletalMesh");

// Transforms (parent-relative/component space)
auto* t = subj->addTransform("Root", -1);
// Fill TRS
// ...
// Ensure TRS is serialized
t->transformOrder = { O3DS::TTranslation, O3DS::TRotation, O3DS::TScale };

// Optional curves
subj->mCurveNames = {"Smile", "EyeBlink_L"};
subj->mCurveValues = {0.8f, 0.2f};

// Serialize full description
list.Serialize(buf /*out*/, /*timestamp*/ 0.0 /*uses GetTime() when 0*/);

// Send buf via selected connector (tcp://, udp://, nng+tcp://, webrtc://)
```

## Transport usage (non-normative)
- Select a connector by URL scheme (see `src/o3ds/base_connector.*`). Connectors expose a common async API (`start`, `write`, `read`, `getError`).
- Auto-reconnect and backpressure are transport concerns in the adapter; the protocol payload is identical regardless of transport.

## Validation and tests
- Round-trip tests in this workspace validate TRS round-trips when `transformOrder` is set:
  - `Open3DStream.M2.RoundTrip.Subject` — two-node hierarchy with curves.
  - `Open3DStream.M2.RoundTrip.SubjectNoCurves` — single-node, no curves.
- Additional core tests: `test_curves.cpp`, `test_curve_comprehensive.cpp`.
- Receiver compatibility: Existing Unreal LiveLink source (`FOpen3DStreamSource`) consumes `SubjectList` and maps to LiveLink subjects.

## Limitations and notes
- No exceptions in hot paths; use boolean results and `getError()` from connectors.
- Do not redefine or fork the schema; all adapters must consume the same `src/o3ds.fbs`-generated headers.
- Timecode: Only `time` (double seconds) is present in SubjectList today; additional timecode fields would require optional schema updates and a version bump.

## Decision summary for Open3DBroadcast
- Transform space: Component-space (parent-relative) bone transforms in the evaluated final pose.
- Coordinate system and units: Unreal defaults (LH, Z-up, centimeters); carry axes in Subject fields.
- Curves: Names sent once in Subject; values per-frame via Subject or SubjectUpdate; index-aligned.
- Timestamps: Include `time` in SubjectList; also capture engine timecode separately in the adapter if needed (not in protocol).
- Backpressure: Latest-wins—drop oldest pending frame if connector write is busy; batch multiple subjects per frame.
- Enforcement in broadcaster: The Unreal broadcaster sets `transformOrder` for every node and calls `CalcMatrices()` prior to serialization to keep internal state consistent.

---

This document is a living reference for agents implementing the Open3DBroadcast module. If schema or adapter conventions change, update this file alongside code changes and bump `O3DS_VERSION_TAG` when schema updates occur.
