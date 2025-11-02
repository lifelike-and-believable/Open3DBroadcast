# Troubleshooting

## Build/Test Sandbox paths

Editor builds and tests run in `ProjectSandbox`, where `ProjectSandbox/Plugins/Open3DStream` is a symbolic link to this repository’s `plugins/unreal/Open3DStream` folder. As a result, build diagnostics (compiler/linker) will often reference files under:

```
ProjectSandbox/Plugins/Open3DStream/...
```

Map these paths back to the repository at:

```
plugins/unreal/Open3DStream/...
```

This is expected and helps keep iteration fast inside the sandbox.

## WebRTC refactor plan

For the ongoing WebRTC connector refactor (Issue #134), see:

- `WEBRTC_CONNECTOR_REFACTOR_PLAN_ISSUE134.md`

It contains milestones, acceptance criteria, and testing notes.

## UE 5.6 API checks

When adding/modifying Unreal-facing code paths, verify UE 5.6 API signatures (WebSockets, audio capture/mixer, skinned mesh post-eval hooks, LiveLink data push) against official docs before committing changes.