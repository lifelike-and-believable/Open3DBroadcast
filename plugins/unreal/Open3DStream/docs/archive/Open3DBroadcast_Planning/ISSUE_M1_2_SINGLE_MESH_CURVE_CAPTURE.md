[M1] Single-Mesh Curve Capture (Morph + Named Anim Curves)

Context and purpose

• Capture animation curves from the same single USkeletalMeshComponent used in M1_1.

• Support morph target weights and named animation curves, conforming to CURVE_SUPPORT.md.

Tasks

• Reuse the Broadcast component/module scaffold from M1_1.

• Capture morph target weights:

• Read active morph targets and current weights after pose evaluation.

• Normalize per CURVE_SUPPORT.md conventions; ensure stable naming.

• Capture animation curves:

• Read named animation curves from the AnimInstance after evaluation.

• Apply filtering, ranges, and naming per CURVE_SUPPORT.md.

• Integrate toggleable debug output:

• CVar (e.g., o3ds.Broadcast.DebugCurves=1) to log a subset of curves each frame.

• Ensure performance safety:

• Avoid allocations per frame; reuse buffers and name maps.

• Throttle logging.

• Validation in ProjectSandbox:

• Use an AnimBP or animation that drives both morph targets and named curves.

• Compare logged curve values to Anim Previewer/AnimBP.

Acceptance criteria

• In PIE, logged morph and named curve values match the Anim Previewer/AnimBP at capture time for the sandbox mesh.

• Naming and ranges match CURVE_SUPPORT.md; no spurious or duplicated curves captured.

• Capture runs at 60 FPS on a modest curve set without visible stalls.

References

• Open3DBroadcast Plan: plugins/unreal/Open3DStream/docs/Open3DBroadcast_Planning/Open3DBroadcast_Planning.md

• CURVE_SUPPORT.md — source of truth for naming, ranges, filtering, and normalization.

• test_curve_comprehensive.cpp, test_curves.cpp — semantics parity.

• ProjectSandbox/README.md

• Unreal Engine API Documentation: https://dev.epicgames.com/documentation/en-us/unreal-engine/API

Dependencies

• ISSUE_M1_1_SINGLE_MESH_POSE_CAPTURE.md — share lifecycle and timing hook for synchronized capture.

• ISSUE_M0_2_CURVE_SEMANTICS.md — adopt naming/range/filtering decisions.

Out of scope and risks

• No O3DS serialization (M2) or transport send (M3).

• Risks: curve read timing pre/post-eval mismatch; morph normalization inconsistencies vs CURVE_SUPPORT.md; unnecessary per-frame allocations.

Evidence

• Log snippets showing representative morph targets and named curves matching Anim Previewer/AnimBP values.

• Note on performance (e.g., simple profiling numbers or absence of hitches).

Labels

• milestone:M1, area:unreal, perf, tests, docs
