# O3D_WITH_TRANSPORT_MOQ Flag Documentation

## Overview

The `O3D_WITH_TRANSPORT_MOQ` environment variable controls whether the MoQ (Media over QUIC) transport module is compiled and linked into the Open3DBroadcast plugin.

## Default Value

**Default:** `true` (MoQ transport is enabled by default on Win64)

> **Platform guard:** The build scripts automatically disable MoQ on non-Win64 targets until additional moq-ffi binaries are vendored. Setting `O3D_WITH_TRANSPORT_MOQ=1` on Linux/macOS will have no effect; the flag is forced off during build configuration with a warning.

## Usage

### Setting the Flag

The flag can be set via environment variable before building:

**Windows (PowerShell):**
```powershell
$env:O3D_WITH_TRANSPORT_MOQ = "1"  # Enable (default)
$env:O3D_WITH_TRANSPORT_MOQ = "0"  # Disable
```

**Windows (Command Prompt):**
```cmd
set O3D_WITH_TRANSPORT_MOQ=1  REM Enable (default)
set O3D_WITH_TRANSPORT_MOQ=0  REM Disable
```

**Linux/Mac (Bash):**
```bash
export O3D_WITH_TRANSPORT_MOQ=1  # Enable (default)
export O3D_WITH_TRANSPORT_MOQ=0  # Disable
```

### Checking the Flag in C++

The flag is exposed as a preprocessor definition:

```cpp
#if O3D_WITH_TRANSPORT_MOQ
    // MoQ transport is available
    UE_LOG(LogTemp, Log, TEXT("MoQ transport enabled"));
#else
    // MoQ transport is not available
    UE_LOG(LogTemp, Log, TEXT("MoQ transport disabled"));
#endif
```

### Checking the Flag in Build.cs

```csharp
using UnrealBuildTool;

public class MyModule : ModuleRules
{
    public MyModule(ReadOnlyTargetRules Target) : base(Target)
    {
        // Check if MoQ transport is enabled
        if (O3DBuildFlags.IsMoQEnabled(Target))
        {
            // MoQ transport is available
            PrivateDependencyModuleNames.Add("Open3DTransportMoQ");
        }
    }
}
```

## Build System Integration

The flag is defined and managed in `Open3DShared/Open3DShared.Build.cs`:

```csharp
internal static class O3DBuildFlags
{
    private sealed class Settings
    {
        // ... other flags ...
        public bool WithMoQ = true;  // Default enabled
    }

    private static Settings Get(ReadOnlyTargetRules Target)
    {
        // ... initialization ...
        Result.WithMoQ = ReadBool("O3D_WITH_TRANSPORT_MOQ", Result.WithMoQ);
        // ...
    }

    // Public API for checking flag
    public static bool IsMoQEnabled(ReadOnlyTargetRules Target) => Get(Target).WithMoQ;
}
```

## Module Behavior

### When Enabled (O3D_WITH_TRANSPORT_MOQ=1)

- **Open3DTransportMoQ module** is compiled and linked
- **moq-ffi library** is loaded at runtime
- **MoQ transport factories** are registered in sender/receiver registries
- **Editor UI customizations** for MoQ configuration are available
- **"MoQ" transport option** appears in dropdown menus

### When Disabled (O3D_WITH_TRANSPORT_MOQ=0)

- **Open3DTransportMoQ module** is skipped during compilation
- **moq-ffi library** is not loaded
- **No MoQ transport factories** are registered
- **No editor UI** for MoQ configuration
- **"MoQ" transport option** does not appear in dropdowns
- **Reduced plugin size** (moq_ffi.dll not packaged)

## Why Disable MoQ Transport?

You might want to disable the MoQ transport if:

1. **Platform not supported:** MoQ FFI binaries not available for your target platform
2. **Licensing concerns:** Want to exclude moq-rs dependencies
3. **Reduce plugin size:** Don't need MoQ features in your project
4. **Build troubleshooting:** Isolate issues during development
5. **Production deployment:** Only using NNG/WebRTC/Sockets transports

## Dependencies

When MoQ transport is enabled:

- **moq-ffi library** must be present in `ThirdParty/moq-ffi/`
- **Rust-compiled binaries** (moq_ffi.dll/so/dylib) must exist
- **Platform support:** Currently Win64 only (Linux/Mac builds disable the flag automatically)

See `ThirdParty/moq-ffi/README.md` for the exact upstream commit, binary hashes, and refresh workflow.

## Related Flags

Other transport flags work similarly:

- `O3D_WITH_TRANSPORT_SOCKETS` - TCP/UDP sockets transport
- `O3D_WITH_TRANSPORT_NNG` - NNG (nanomsg) transport
- `O3D_WITH_TRANSPORT_WEBRTC` - WebRTC (LiveKit/libdatachannel) transport

All transport flags default to `true` (enabled).

## Build Examples

### Disable MoQ, enable others:
```powershell
$env:O3D_WITH_TRANSPORT_MOQ = "0"
# Build UE project
```

### Enable only MoQ (disable others):
```powershell
$env:O3D_WITH_TRANSPORT_SOCKETS = "0"
$env:O3D_WITH_TRANSPORT_NNG = "0"
$env:O3D_WITH_TRANSPORT_WEBRTC = "0"
$env:O3D_WITH_TRANSPORT_MOQ = "1"
# Build UE project
```

### Default (all transports enabled):
```powershell
# No environment variables needed - all enabled by default
# Build UE project
```

## Troubleshooting

### Error: "Missing required MoQ FFI library"

**Symptom:** Build fails with error about missing `moq_ffi.dll.lib`

**Solution:**
1. Confirm you are targeting **Win64**. Other platforms will disable the transport automatically.
2. Verify `O3D_WITH_TRANSPORT_MOQ=1` (or unset for default)
3. Ensure the files listed in `ThirdParty/moq-ffi/README.md` exist and their hashes match
4. Rebuild after copying the binaries, or temporarily disable MoQ via `$env:O3D_WITH_TRANSPORT_MOQ = "0"`

### Error: "moq_ffi.dll not found" at runtime

**Symptom:** Editor/game fails to start with DLL not found error

**Solution:**
1. Ensure DLL exists in `ThirdParty/moq-ffi/bin/Win64/Release/moq_ffi.dll` (match SHA256 from README)
2. Check that `RuntimeDependencies` in Build.cs includes the DLL
3. Verify `O3D_WITH_TRANSPORT_MOQ=1` during build (and that you built for Win64)
4. Clean and rebuild the project

### MoQ transport not appearing in dropdown

**Symptom:** "MoQ" option missing from transport selection

**Solution:**
1. Check that `O3D_WITH_TRANSPORT_MOQ=1` (or unset)
2. Rebuild the plugin with correct flag
3. Check Output Log for "MoQ transport module started" message
4. Verify no errors during `FMoQFfiSupport::LoadLibrary()`

## See Also

- `ThirdParty/moq-ffi/README.md` - MoQ FFI library setup and refresh guide
- `Open3DShared/Open3DShared.Build.cs` - Build flags implementation
- `Open3DTransportMoQ/Open3DTransportMoQ.Build.cs` - Module build configuration
- `MOQ_TRANSPORT_IMPLEMENTATION_PLAN.md` - Full implementation plan
