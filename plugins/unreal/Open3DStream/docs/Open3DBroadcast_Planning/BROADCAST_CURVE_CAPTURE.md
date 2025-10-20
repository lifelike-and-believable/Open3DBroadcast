# Open3DBroadcast — Curve Capture, Semantics, and Filtering (M0.2)

This document defines how Open3DBroadcast in Unreal should capture, normalize, filter, and send animation curves (morph targets and named animation curves) through the Open3DStream protocol.

Related references:
- Protocol and LiveLink curve support: `plugins/unreal/Open3DStream/docs/CURVE_SUPPORT.md`
- Protocol reference (broadcast mapping): `plugins/unreal/Open3DStream/docs/Open3DBroadcast_Planning/PROTOCOL_REFERENCE.md`
- Tests: `test_curves.cpp`, `test_curve_comprehensive.cpp`
- Planning task: `ISSUE_M0_2_CURVE_SEMANTICS.md`

## Scope and goals

- Capture final evaluated curve values each frame for registered `USkeletalMeshComponent` subjects.
- Normalize and clamp where appropriate to maintain predictable ranges.
- Filter curves to reduce bandwidth while preserving fidelity.
- Maintain compatibility with existing receivers (Unreal LiveLink source, Maya, MotionBuilder) and the protocol schema.

## Curve types and naming

We support two sources of curve data from Unreal:

- Morph Targets (Blend Shapes)
  - Source: `USkeletalMeshComponent` evaluated morph target weights
  - Protocol name: exact morph target name (FName.ToString())
  - Typical value range: [0.0, 1.0]

- Animation Curves (Named Curves)
  - Source: `UAnimInstance` evaluated animation curve container
  - Protocol name: exact curve name (FName.ToString())
  - Typical value range: depends on authoring; often [0.0, 1.0], but may be unbounded

Name policy:
- Preserve exact Unreal names for both morph targets and named curves.
- Names are case-sensitive in Unreal; protocol consumers should treat them as case-sensitive identifiers.
- If a morph target and an animation curve share the same name, prefer a single stream where the actively evaluated source value wins for that frame, or optionally prefix one source via settings (configurable; default is no prefix and last-writer-wins per evaluation order).

## Value ranges and normalization

- Morph targets:
  - Clamp to [0.0, 1.0] before sending.
  - If a negative value is encountered, clamp to 0.0.
  - If value > 1.0 (due to authoring or blend), clamp to 1.0.

- Animation curves:
  - Pass-through by default (no clamping), because semantics are app-defined.
  - Optional per-source clamp/range normalization may be configured (e.g., [0,1] or [-1,1]).

- NaN/Inf handling:
  - If a curve value is NaN or infinite, drop the curve for that frame (do not transmit) and optionally log a throttled warning.

## Filtering rules (latest-wins)

Purpose: reduce bandwidth and noise while keeping meaningful expressivity.

- Zero/default filter:
  - Optionally skip curves whose absolute value <= epsilon (default epsilon = 1e-6) to avoid streaming constant zeros.

- Delta threshold:
  - Only include a curve in an update if |current - last_sent| >= deltaThreshold.
  - Recommended defaults: 0.01 for [0,1] curves; 0.1 for unbounded curves. Configurable.

- Include/exclude patterns:
  - Allow wildcard lists for include and exclude (e.g., `Face/*`, `Debug/*`). Exclude wins over include.

- Max curve count per subject:
  - Configurable hard cap to protect bandwidth (e.g., 1024). If exceeded, sort by magnitude of change and send top-N.

- Descriptor vs update:
  - Send curve names once in the Subject descriptor (order must be stable). Then send values in the same index order per frame. If the set of names changes, resend a new descriptor before subsequent updates.

- Backpressure:
  - If the connector is backlogged, drop the oldest pending frame (latest-wins) and coalesce multiple subjects into one message per frame.

## Unreal extraction — practical guidance

This section outlines robust, version-tolerant patterns to read curves after animation evaluation on the game thread.

- Capture timing:
  - Schedule capture after animation evaluation for each registered `USkeletalMeshComponent` (e.g., in a subsystem tick or appropriate post-eval hook). Read the final evaluated values.

- Morph targets (weights):
  - Unreal does not expose a stable public API on `USkeletalMeshComponent` to query morph target weights by name across all versions.
  - Recommended approach: use the `UAnimInstance` evaluated curve container for morph-driving curves. Ensure your curve discovery includes morph target names so `GetCurveValue(Name)` returns the current driving value.
  - If direct per-target weights are required, consider engine-specific access patterns or custom animation notifies to surface values; keep this out of hot paths to avoid version fragility.

- Animation curves (named curves):
  - Read evaluated curves from the active `UAnimInstance`.
  - Enumeration strategies vary by UE version; two practical approaches:
    1) If you maintain a discovered-name set, update it lazily when new curves appear; each frame query `AnimInstance->GetCurveValue(Name)`.
    2) Enumerate names using the skeleton’s SmartName container (Anim Curve Mapping) and read `GetCurveValue` for each.
  - Pseudocode:

    ```cpp
  UAnimInstance* AnimInst = SkelComp ? SkelComp->GetAnimInstance() : nullptr;
    if (AnimInst)
    {
        // Strategy 1: known names (discovered earlier or configured)
        for (const FName& CurveName : KnownAnimCurveNames)
        {
            const float V = AnimInst->GetCurveValue(CurveName);
            AddCurve(CurveName.ToString(), V); // optional clamp via settings
        }

        // Strategy 2: enumerate from skeleton mapping (setup step)
        // USkeleton* Skeleton = SkelComp->GetSkeletalMeshAsset()->GetSkeleton();
        // const FSmartNameMapping* Mapping = Skeleton->GetSmartNameContainer(USkeleton::AnimCurveMappingName);
        // Mapping->Iterate([&](const FSmartName& SmartName, const FName& DisplayName){ KnownAnimCurveNames.Add(DisplayName); });
    }
    ```

- Index alignment and stability:
  - Build a stable vector of curve names per Subject. Maintain the same order across frames.
  - When the set of curves changes (e.g., a new morph appears), rebuild the descriptor and resend before updates.

## Protocol mapping and batching

- Names are stored once in the `Subject` descriptor (`mCurveNames`), with values in `mCurveValues` aligned by index.
- Per-frame updates use `SubjectUpdate` with `CurveUpdate` entries by index where values changed (delta-based), as implemented in the core model.
- Batch multiple subjects each frame into a single `SubjectList` with a `time` stamp for efficient delivery.

See: `src/o3ds/model.h/.cpp` and tests for concrete packing and parsing behavior.

## Configuration recommendations (defaults)

- Enable curve streaming: true
- Morph target clamping: [0,1]
- Animation curve clamping: off (pass-through)
- Delta threshold: 0.01 (bounded curves), 0.1 (unbounded)
- Zero filter epsilon: 1e-6
- Include patterns: empty (include all)
- Exclude patterns: `Debug/*`, `Internal/*` (optional)
- Max curves per subject: 1024

These should be exposed via `UO3DBroadcastSettings` and/or per-component overrides in `UO3DBroadcastComponent`.

## Examples

- Morph target example:

  | Name      | Value | Action               |
  |-----------|-------|----------------------|
  | Smile_L   | 1.20  | Clamp to 1.0         |
  | EyeBlinkR | -0.10 | Clamp to 0.0         |

- Anim curve example:

  | Name          | Value | Action                   |
  |---------------|-------|--------------------------|
  | HeartRate     | 120.0 | Pass-through (unbounded) |
  | LookAtWeight  | 0.5   | Pass-through             |

- Filtering example (delta threshold = 0.01):

  Previous Smile = 0.70, Current = 0.705 → delta 0.005 < 0.01 → skip in update

## Troubleshooting

- Curves missing in receivers:
  - Verify the Subject descriptor includes curve names; updates reference indices from this list.
  - Ensure names match exactly; check case and spelling.

- Curves never update:
  - Check delta threshold; set to 0.0 temporarily to validate.
  - Verify extraction timing occurs after evaluation on the game thread.

- Excessive bandwidth:
  - Increase delta threshold, enable zero filter, reduce max curves per subject, or constrain include patterns.

## Alignment with existing docs and tests

- Matches `CURVE_SUPPORT.md` structure and the packing/parse patterns validated by `test_curves.cpp` and `test_curve_comprehensive.cpp`.
- Keeps protocol stable: names set in descriptor, values aligned by index, updates carry only changed indices.

---

This is a living document for the Open3DBroadcast module. As implementation lands, update examples and configuration defaults accordingly and ensure any schema changes are coordinated with a version bump and regenerated FlatBuffers headers.
