# Broadcast Protocol Reference (M0.1)

Scope
- Defines how Unreal broadcast uses the existing Open3DStream (O3DS) model API to send skeleton, transforms, and curves.
- No schema changes. Uses `o3ds/model.h` and generated `o3ds_generated.h`.

Message model
- Top-level: `O3DS::SubjectList`
  - Fields: `mTime` (double seconds), vector of `Subject`.
- Subject: `O3DS::Subject`
  - `mName` (std::string)
  - `mTransforms` (vector of `Transform*`) with `mParentId` and TRS component containers
  - Curves: `mCurveNames` (vector<string>), `mCurveValues` (vector<float>) aligned by index
  - Helpers: `addSubject(name)`, `addTransform(name, parentId)`, `CalcMatrices()`

Unreal ? O3DS mapping
- Subject name: sanitized UE subject ? `Subject.mName`
- Bones: reference-skeleton order
  - `FO3DSSkeletonDescriptor.BoneNames[i]` ? `Subject.addTransform(name, parent)`
  - `FO3DSSkeletonDescriptor.ParentIndices[i]` ? `parent`
- Per-frame transforms (parent-relative)
  - `FTransform.Translation` ? `transform.translation.value` (XYZ)
  - `FTransform.Rotation` (FQuat) ? `transform.rotation.value` (XYZW)
  - `FTransform.Scale3D` ? `transform.scale.value` (XYZ)
- Curves (per frame)
  - `FO3DSPoseFrame.CurveNames[j]` ? `Subject.mCurveNames[j]`
  - `FO3DSPoseFrame.CurveValues[j]` ? `Subject.mCurveValues[j]`

Timing
- Serialized with `FPlatformTime::Seconds()` as `SubjectList.mTime`.
- Receiver uses `mTime` to populate `FLiveLinkBaseFrameData.MetaData.SceneTime`.

Compatibility and limits
- Bone count: sized by reference skeleton; frames are clamped to current required bone count.
- Curve count: determined by component cache; see CURVE_CAPTURE doc.
- Message size: full TRS + full curves per frame in M2; no compression by default.

References
- `Plugins/Open3DStream/Source/Open3DBroadcast/Private/O3DSBroadcastSerializer.cpp`
- `Plugins/Open3DStream/Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`
- Protocol: `src/o3ds.fbs`, `src/o3ds/model.h`
