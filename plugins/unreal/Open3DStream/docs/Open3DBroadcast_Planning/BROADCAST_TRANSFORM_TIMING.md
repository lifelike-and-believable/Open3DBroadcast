# Open3DBroadcast — Transform Space, Coordinate System, and Timing (M0.3)

This document records explicit decisions for Open3DBroadcast regarding transform space, coordinate system, units, rotation representation, and timing.

Related references:

- Protocol schema: `src/o3ds.fbs` and generated `src/o3ds_generated.h`
- Model helpers: `src/o3ds/model.h`, `src/o3ds/model.cpp`
- Unreal receiver reference: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/`
- Planning task: `ISSUE_M0_3_TRANSFORM_SPACE.md`
- Protocol reference: `PROTOCOL_REFERENCE.md`

## Decisions (summary)

- Transform space: Parent-relative (bone-space within the component hierarchy), aligned to the Subject’s node order.
- Coordinate system: Send Unreal as-is (left-handed, Z-up). Carry axes in Subject metadata (x_axis, y_axis, z_axis).
- Units: Centimeters (Unreal native). No conversion in protocol.
- Rotation: Quaternion (x, y, z, w) as defined in the protocol `Rotation` struct. Normalize before send.
- Timing: `SubjectList.time` = monotonic seconds; optionally capture Unreal timecode in the adapter UI/logs.

These choices align with existing receivers (Unreal LiveLink source) and keep the protocol transport-agnostic.

## Rationale

- Parent-relative transforms are stable across world movement and match typical animation pipelines; receivers already use hierarchy to rebuild component/world space as needed.
- Sending Unreal’s coordinate system directly avoids redundant conversions; receivers like Maya adapters already perform axis mapping.
- Centimeters preserve Unreal asset fidelity; consumers can convert to meters if desired.
- Quaternions are robust for animation; the protocol’s `Rotation` is 4 floats in (x,y,z,w).
- A single monotonic timestamp suffices for ordering and latency; editorial timecode remains adapter-side for now.

## Extraction from Unreal

Capture after final pose evaluation on the game thread, per registered `USkeletalMeshComponent`.

- Build a stable node list (bone names + parent indices) from the skeleton and store in the Subject descriptor.
- Each frame, read evaluated component-space transforms, then compute parent-relative for streaming.

Pseudocode:

```cpp
USkeletalMeshComponent* C = /* registered */;
// Build/keep a stable bone name + parent index list (from skeleton) once per Subject

const TArray<FTransform>& CompSpace = C->GetComponentSpaceTransforms();

for (int32 BoneIndex = 0; BoneIndex < CompSpace.Num(); ++BoneIndex)
{
    const int32 ParentIndex = GetParentIndex(BoneIndex); // from skeleton ref pose
    FTransform Rel;
    if (ParentIndex >= 0)
    {
        Rel = CompSpace[BoneIndex].GetRelativeTransform(CompSpace[ParentIndex]);
    }
    else
    {
        Rel = CompSpace[BoneIndex]; // root relative to component origin
    }

    const FVector T = Rel.GetTranslation();      // centimeters
    const FQuat   Q = Rel.GetRotation().GetNormalized(); // (x,y,z,w)
    const FVector S = Rel.GetScale3D();          // per-axis scale

    AddTransform(/*i=*/BoneIndex, /*parent=*/ParentIndex, /*name=*/BoneNames[BoneIndex], T, Q, S);
}
```

Notes:

- Maintain index alignment with the Subject descriptor’s nodes; resend descriptor if the skeleton changes.
- Normalize quaternions to avoid drift; avoid mixing spaces within the same frame.

## Coordinate system and axes

- Unreal LH Z-up is sent as-is.
- Subject metadata should include axes (x_axis, y_axis, z_axis) for explicit orientation.
- Receivers perform mapping (e.g., to RH Y-up for Maya) during ingest.

## Units

- Distances: centimeters.
- Scales: unitless per-axis.
- If receivers require meters, apply 0.01 scaling on their side.

## Rotation representation

- Use protocol `Rotation` struct: four floats in order (x, y, z, w).
- Unreal’s `FQuat` exposes `X,Y,Z,W`; export directly in that order after normalization.

## Timing and synchronization

- `SubjectList.time`: double seconds from a monotonic clock (e.g., `O3DS::GetTime()` in the core model) for ordering and latency.
- Unreal timecode (optional): capture `FQualifiedFrameTime` in the adapter for UI/logging; not currently a protocol field. If added later, must be optional with a version bump.
- Frame number: derive locally in receivers if needed; protocol relies on time + arrival sequencing.

Example:

```cpp
double Timestamp = O3DS::GetTime(); // Monotonic seconds for SubjectList.time
// Optionally:
// FQualifiedFrameTime QFT = UGameplayStatics::GetTimecode(/*World*/);
```

## Validation

- Visual: compare parent-relative sampled transforms against Unreal’s animation debugger/previewer.
- Round-trip: send to an Unreal receiver instance and verify motion matches.
- Numeric: ensure applying the same hierarchy reconstructs expected component/world space.

## Troubleshooting

- Flipped bones/orientations: verify no send-side coordinate conversion; receivers map axes.
- Jitter/drift: ensure capture occurs after evaluation; normalize quats; avoid switching spaces.
- Skeleton mismatches: resend descriptor when bone set/order changes.

---

These decisions guide the M1 implementation. Protocol changes would require schema updates, FlatBuffers regeneration, and a version tag bump (`O3DS_VERSION_TAG`).
