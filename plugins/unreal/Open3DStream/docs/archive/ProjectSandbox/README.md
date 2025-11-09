# ProjectSandbox — Broadcast Component Validation

Steps to validate UO3DSBroadcastComponent:

- Open ProjectSandbox.uproject (UE 5.4/5.5 configured under usr/).
- In a test level, place a Skeletal Mesh Actor (e.g., Mannequin).
- Add UO3DSBroadcastComponent to the actor.
- Assign TargetMesh (or leave auto-discover if only one SkeletalMeshComponent on the actor).
- In Play In Editor (PIE):
  - Execute: `o3ds.Broadcast.DebugPose 1` in the console to enable logs.
  - Call StartCapture via Blueprint or set AutoStart in future UI (for now, call StartCapture from BeginPlay or console via script).
  - Verify log output shows frame counter and first few bone transforms.
- Check values against the Animation Previewer/AnimBP debugger for the same frame; they should match parent-relative (component hierarchy) per M0.3.

Tuning:

- Use `CaptureRateHz` to throttle logs (e.g., 30) when profiling.
- Disable logs with `o3ds.Broadcast.DebugPose 0`.

Notes:

- This is a minimal M1 validation harness; networking/serialization are out of scope here.
