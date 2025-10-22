# Broadcast Transform Space and Timing (M0.3)

Transform space
- Descriptor built from `USkeletalMesh` reference skeleton (index and parent arrays)
- Per-frame transforms are parent-relative
  - Derived from component-space: `Rel = CS[i].GetRelativeTransform(CS[parent])`, or `Rel = CS[i]` if no valid parent
  - Quaternion normalized each frame

Timing
- Capture occurs after pose evaluation using `USkinnedMeshComponent::RegisterOnBoneTransformsFinalizedDelegate`
- Rate limiting via `CaptureRateHz` (<= 0 means capture every evaluation)
- Serialized timestamp set to `FPlatformTime::Seconds()` and forwarded in the serialized buffer

Name policy
- Subject name synthesized as `World/Actor/Component`, then sanitized to `[-._A-Za-z0-9/]` and whitespace replaced with `_`

LOD and required bones
- Component-space transform count may differ from reference skeleton due to LOD
- Frames are clamped to min(CompSpace.Num, RefSkeleton.Num)
- Descriptor is not rebuilt due to LOD; descriptor re-sent on actual mesh/skeleton change

Validation
- Debug CVars: `o3ds.Broadcast.DebugPose`, `o3ds.Broadcast.DebugCurves`
- Stats/diagnostics: use `o3ds.Broadcast.Transport.DumpStats` and `o3ds.Broadcast.Transport.DumpTransportStats` to inspect queues and counters.

References
- `Plugins/Open3DStream/Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`
- `Plugins/Open3DStream/Source/Open3DBroadcast/Private/O3DSBroadcastSerializer.cpp`
