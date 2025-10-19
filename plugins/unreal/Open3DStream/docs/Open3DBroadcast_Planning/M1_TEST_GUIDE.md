# Open3DBroadcast — M1 Test Guide (Single‑Mesh Capture)

This guide walks you through validating the M1 milestone for Open3DBroadcast: capturing a single skeletal mesh’s final evaluated pose and curves inside Unreal and verifying correctness via logs.

Applies to: UE 5.6, plugin path `plugins/unreal/Open3DStream`.

Related docs:
- Transform & timing decisions: `BROADCAST_TRANSFORM_TIMING.md`
- Curve capture semantics: `BROADCAST_CURVE_CAPTURE.md`
- Protocol reference (for future milestones): `PROTOCOL_REFERENCE.md`

---

## 1) Prerequisites

- Unreal Engine 5.6 installed.
- This plugin present in your project (or use `ProjectSandbox/ProjectSandbox.uproject` in the repo).
- A skeletal mesh asset with:
  - A valid skeleton and an animation source (Anim Blueprint or Anim Sequence) that drives motion.
  - One or more morph targets (e.g., `Smile_L`, `EyeBlink_R`) to validate morph weight capture.

Optional:
- Familiarity with the Output Log window and in-editor console.

---

## 2) Enable and open

1. Copy `plugins/unreal/Open3DStream` into your project’s `Plugins/` directory (if not using `ProjectSandbox`).
2. Open your project (or `ProjectSandbox.uproject`).
3. Ensure the plugin “Open3DStream” is enabled; it includes both Receiver and Broadcast modules.

---

## 3) Set up a test actor

1. Create a new empty Actor Blueprint (e.g., `BP_M1_Test`).
2. Add a `SkeletalMeshComponent` and assign your test skeletal mesh.
3. Add `O3DSBroadcastComponent` to the actor.
4. Optionally set `CaptureRateHz` on the broadcast component (e.g., `60.0`).
5. In the Blueprint’s Event Graph, call `StartCapture` on the `O3DSBroadcastComponent` during `BeginPlay`.
   - This ensures capture starts automatically when you press Play.

Place `BP_M1_Test` in a test level.

---

## 4) Enable debug logs (for inspection)

Open the Output Log and the in‑editor console (~) and run:

- `o3ds.Broadcast.DebugPose 1` — logs per‑frame pose header and first few bones.
- `o3ds.Broadcast.DebugCurves 1` — logs first few curves and values.

Tip: Disable with `… 0` when you’re done.

---

## 5) Run and inspect

1. Press Play (PIE). You should see a line similar to:
   - `Broadcast capture bound to SkeletalMeshComponent…`
2. Each frame (subject to `CaptureRateHz`), expect logs like:
   - `[O3DS] Pose #<n> Subject=<World>/<Actor>/<Component> Bones=<count>`
   - Followed by a few lines of bone transforms in parent‑relative space:
     - `T(x,y,z)`, `Q(x,y,z,w)`, `S(x,y,z)`
3. Curves log prints the first few curve names and values.

What to look for:
- Subject ID formatting: Should contain no spaces or special characters (slashes only as separators). Example: `MyWorld/MyActor/SkeletalMeshComponent`.
- Bone count matches your skeleton; first few entries change with animation.
- Quaternions appear normalized (values |x|,|y|,|z| ≤ ~1; w present).
- Curves include morph target names (e.g., `Smile_L`) and named anim curves. Morph values are clamped to [0,1].

---

## 6) Validate capture timing (post‑evaluation)

The broadcast component binds to `USkinnedMeshComponent::OnBoneTransformsFinalized` to capture after animation evaluation.

Suggested checks:
- Scrub an Anim Sequence or play a looping Anim Blueprint. Pose logs should reflect the final evaluated pose each frame.
- Pause PIE and step a single frame; expect exactly one pose log advance and curves reflecting that step.

If you momentarily disable the delegate and call capture from Tick (not recommended), you might observe occasional mismatches—this is the reason we bind to the post‑eval hook.

---

## 7) Validate morph target capture

Ways to drive morphs during PIE:
- In your Anim Blueprint, use `Set Morph Target` nodes to animate a morph (e.g., oscillate `Smile_L` between 0 and 1).
- Drive morphs via an Anim Curve that your ABP maps into morph weight.

Expected behavior:
- Morph names appear in the curve list.
- Values reflect current component morph weights.
- Values are clamped to [0, 1].
- If an Anim Curve drives the same name, the Anim value will override (last‑writer‑wins policy).

---

## 8) Edge cases

- Swap skeletal mesh at runtime (e.g., in BP) and observe log: skeleton cache refresh should occur; bone count may change accordingly.
- Enable/disable `CaptureRateHz` (set ≤ 0 to capture every evaluation); logs should throttle accordingly.
- Call `StopCapture` at runtime; logs cease and unbind should be logged.

---

## 9) Troubleshooting

- No logs at all:
  - Ensure `StartCapture` is called (BeginPlay or manual call).
  - Confirm `o3ds.Broadcast.DebugPose` and/or `…DebugCurves` are set to 1.
- Subject name shows spaces/special characters:
  - Ensure you are on the M1 hardening build; names are sanitized internally.
- Curves missing:
  - Verify your Anim Blueprint evaluates curves and/or morphs.
  - Ensure the skeleton’s SmartName mapping contains your curve names (for named curves).
- Values outside [0,1] for morphs:
  - Morph weights are clamped in the broadcaster; if you see otherwise, check which name is being logged (could be an anim curve).

---

## 10) Success criteria (M1)

- Deterministic timing: Pose and curve logs reflect final evaluated animation per frame (post‑evaluation capture).
- Subject ID policy: IDs are slash‑separated; ASCII; no spaces; limited to `[-._A-Za-z0-9/]`.
- Transform space: Parent‑relative (bone‑space) consistent with the skeleton.
- Curves: Morphs + named curves present; morphs clamped to [0,1].
- Performance: No hot‑path logging unless CVars enabled; disabling CVars silences per‑frame logs.

---

## 11) What’s next (M2+)

- Serialize captured data into `O3DS::Subject/SubjectList` and stream over connectors.
- Include monotonic `SubjectList.time` and axes metadata when building descriptors.
- Add filtering (delta thresholds, zero filter) for curves and transforms.
