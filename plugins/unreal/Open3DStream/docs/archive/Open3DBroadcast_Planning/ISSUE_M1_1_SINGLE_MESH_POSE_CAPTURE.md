[M1] Single-Mesh Pose Capture Module

Context and purpose

• Implement a minimal Broadcast component/module in the Unreal plugin to capture the final evaluated pose for a single USkeletalMeshComponent.

• Establish stable bone identity (names and parent indices) and capture transforms in the chosen transform space per M0 decisions.

Tasks

• Create a Broadcast component/module under plugins/unreal/Open3DStream.

• Suggested: UActorComponent (e.g., UO3DSBroadcastComponent) that binds to a target USkeletalMeshComponent.

• Provide simple Start/Stop capture controls (C++ API; editor UX is M6).

• Hook into post-evaluation to read final bone transforms once per frame.

• Bind to USkinnedMeshComponent::OnBoneTransformsFinalized (or equivalent post-eval hook in UE 5.4) to avoid mid-update reads.

• Ensure reads occur on a safe thread and avoid stalls; initial version may run on game thread.

• Extract skeleton description for the target mesh:

• Stable bone names and parent indices (from RefSkeleton).

• Cache description and only refresh on mesh/LOD/skeleton change.

• Capture per-frame transforms:

• Capture bone transforms in the approved transform space and units (see Dependencies).

• Ensure consistent root handling and parent-space/global-space behavior per decision doc.

• Add debug logging/CSV/console output for verification:

• Per-frame: subject name, frame index/timecode, first N bone transforms.

• Toggle via a CVar (e.g., o3ds.Broadcast.DebugPose=1).

• Add minimal configuration:

• Select target USkeletalMeshComponent (direct reference or path).

• Optional: Update rate (Hz) to throttle capture for early profiling.

• Basic validation harness in ProjectSandbox:

• Place a skeletal mesh actor; attach the Broadcast component; run in PIE and log output.

Acceptance criteria

• In PIE, logs demonstrate captured bone transforms matching Anim Previewer/AnimBP at capture time for a test mesh in ProjectSandbox.

• Stable skeleton description (names and parent indices) is produced and cached; refresh occurs on mesh/skeleton change.

• No visible game-thread hitches during simple capture at 60 FPS for a modest skeleton (e.g., 100 bones).

• Transform space and units match the decisions in M0 Transform Space doc.

References

• Open3DBroadcast Plan: plugins/unreal/Open3DStream/docs/Open3DBroadcast_Planning/Open3DBroadcast_Planning.md

• Curve semantics (for context, used by M1_2): CURVE_SUPPORT.md

• Transform space decisions: ISSUE_M0_3_TRANSFORM_SPACE.md

• ProjectSandbox/README.md

• Unreal Engine API Documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/API

• UE 5.4 API: USkinnedMeshComponent::OnBoneTransformsFinalized (post-evaluation hook)

Dependencies

• ISSUE_M0_3_TRANSFORM_SPACE.md — adopt the chosen transform space, coordinate system, and timing notes.

• ISSUE_M0_1_PROTOCOL_ALIGNMENT.md — naming consistency for future Subject mapping (no serialization in this issue).

Out of scope and risks

• No O3DS serialization or transport send (M2/M3).

• No multi-mesh capture (M4).

• Risks: incorrect hook timing leading to pre-eval or mid-eval reads; transforms not matching previewer due to space/mirroring/retargeting.

Evidence

• Log snippets showing several representative bones with transforms matching Anim Previewer/AnimBP.

• Screenshot or short note demonstrating smooth runtime with no noticeable stalls.

Labels

• milestone:M1, area:unreal, perf, tests, docs
