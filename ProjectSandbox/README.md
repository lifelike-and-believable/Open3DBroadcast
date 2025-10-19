# ProjectSandbox

This is a minimal Unreal Engine project used for testing and developing the Open3DStream plugin.

## Purpose

The ProjectSandbox serves as:
- **Development environment** for the plugin
- **Test harness** for automation tests
- **Integration testing** platform
- **CI/CD build validation**

## Setup

### Quick Start (Windows)

```powershell
# Link the plugin into the sandbox
.\Build\Scripts\Link-PluginIntoSandbox.ps1

# Open the project in Unreal Editor
# The plugin will be automatically loaded and enabled
```

### Quick Start (Linux/Mac)

```bash
# Link the plugin into the sandbox
./Build/Scripts/link_plugin_into_sandbox.sh

# Open the project in Unreal Editor
```

## Structure

```
ProjectSandbox/
├── ProjectSandbox.uproject    # Project file
├── Config/                    # Project configuration
│   ├── DefaultEngine.ini
│   ├── DefaultGame.ini
│   └── DefaultEditor.ini
├── Content/                   # Project content (minimal)
└── Plugins/                   # Plugin symlink (created by Link script)
    └── Open3DStream/          # -> ../../plugins/unreal/Open3DStream
```

## Plugin Linking

The plugin is **not** copied into the project. Instead, a symbolic link (junction on Windows) is created:

- **Windows**: Junction created with `mklink /J`
- **Linux/Mac**: Symbolic link created with `ln -s`

This allows:
- ✅ Single source of truth for plugin code
- ✅ Changes immediately reflected in the project
- ✅ No need to sync changes between locations
- ✅ Git ignores the linked directory

## Testing

### Automation Tests

Run automation tests using the provided script:

```powershell
.\Build\Scripts\Run-AutomationTests.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -TestFilter "Open3DStream.*"
```

### Manual Testing

1. Open `ProjectSandbox.uproject` in Unreal Editor
2. The Open3DStream plugin will be loaded
3. Attach `UO3DSBroadcastComponent` to an actor with a SkeletalMeshComponent (or add via Blueprints)
4. In the component, set `TargetMesh` if not auto-discovered; call `StartCapture()` (via Blueprint or details button if exposed)
5. For transform debug, enable: `o3ds.Broadcast.DebugPose 1`
6. For curve debug, enable: `o3ds.Broadcast.DebugCurves 1`
7. Play in Editor (PIE) with an AnimBP that drives a few morph targets and named animation curves
8. Observe Output Log:

- Pose lines for a few bones each frame
- Curve lines like: `Curve[i] Name = Value`

1. Compare curve values against the Anim Previewer/AnimBP at the same time to verify correctness

## CI/CD Usage

The CI/CD workflows use this sandbox project for:

1. **Build Validation** - Ensures the plugin loads correctly
2. **Automation Tests** - Runs headless tests (optional)
3. **Gauntlet Tests** - Comprehensive integration tests (optional)

See `.github/workflows/` for workflow configurations.

## Notes

- **Minimal Content**: This project contains minimal content to keep repository size small
- **Engine Version**: Configured for UE 5.4 (update in .uproject if needed)
- **Debugging CVars**:
  - `o3ds.Broadcast.DebugPose` (0/1) logs a few parent-relative bone transforms
  - `o3ds.Broadcast.DebugCurves` (0/1) logs a few curve names and values per frame
- **No Game Code**: This is a plugin-only project with no custom game modules
- **Git Ignored**: The `Plugins/` directory is git-ignored since it's a symlink

## Troubleshooting

### Plugin Not Found

If the plugin doesn't appear:

1. Run the Link script again
2. Verify the symlink exists: `ProjectSandbox/Plugins/Open3DStream`
3. Check that it points to `plugins/unreal/Open3DStream`

### Build Errors

If you get build errors:

1. Ensure the plugin source is up to date
2. Delete `Saved/`, `Intermediate/`, and `Binaries/` directories
3. Re-run the Link script
4. Regenerate project files (right-click .uproject → Generate Visual Studio project files)

### Version Mismatch

If UE complains about version:

1. Update `EngineAssociation` in `ProjectSandbox.uproject`
2. Match it to your installed UE version (e.g., "5.4", "5.5")
