# Building O3DS Core as Prebuilt Static Libraries

This document describes how to build and update the O3DS core prebuilt static libraries used by the Unreal plugin.

## Overview

The O3DS core library is now treated as a prebuilt dependency for the Unreal plugin, similar to libdatachannel. This approach provides:
- Consistent builds across platforms
- Faster CI/plugin builds (no need to build O3DS core every time)
- Clear separation between core library and Unreal plugin code
- Better version control of dependencies

## Workflow: Build O3DS Core

The workflow is located at `.github/workflows/build-o3ds-core.yml` and builds O3DS core for all platforms:
- **Windows**: Uses self-hosted runner (ensures toolchain compatibility with UE)
- **Linux**: Uses ubuntu-latest GitHub-hosted runner
- **macOS**: Uses macos-latest GitHub-hosted runner

### Running the Workflow

1. Navigate to the **Actions** tab in GitHub
2. Select **"Build O3DS core"** workflow from the list
3. Click **"Run workflow"** button
4. Wait for the workflow to complete (typically 5-10 minutes)

### Downloading and Committing Artifacts

After the workflow completes successfully:

1. Go to the workflow run page
2. Scroll to the **Artifacts** section at the bottom
3. Download all three artifacts:
   - `o3ds-core-windows`
   - `o3ds-core-linux`
   - `o3ds-core-macos`

4. Extract each artifact and verify the structure:
   ```
   o3ds-core-windows/
   ├── include/
   │   ├── o3ds/           # Public O3DS headers
   │   └── o3ds_generated.h
   └── lib/
       └── Win64/Release/o3ds.lib
   ```

5. Copy the artifacts to the plugin ThirdParty directory:
   ```bash
   # From Windows artifact
   cp -r o3ds-core-windows/* plugins/unreal/Open3DStream/ThirdParty/o3ds/
   
   # From Linux artifact  
   cp -r o3ds-core-linux/lib/* plugins/unreal/Open3DStream/ThirdParty/o3ds/lib/
   
   # From macOS artifact
   cp -r o3ds-core-macos/lib/* plugins/unreal/Open3DStream/ThirdParty/o3ds/lib/
   ```

6. Verify all platform libraries are present:
   ```bash
   ls -lh plugins/unreal/Open3DStream/ThirdParty/o3ds/lib/
   # Should show:
   # Win64/Release/o3ds.lib
   # Linux/Release/libo3ds.a
   # Mac/Release/libo3ds.a
   ```

7. Create a BUILD_INFO.txt file documenting the build:
   ```bash
   cd plugins/unreal/Open3DStream/ThirdParty/o3ds/
   cat > BUILD_INFO.txt << EOF
   O3DS Core Libraries
   Built: $(date)
   Commit: $(git rev-parse HEAD)
   Workflow Run: (paste GitHub Actions run URL)
   Version: $(git describe --tags --always)
   EOF
   ```

8. Commit the libraries to the repository:
   ```bash
   git add plugins/unreal/Open3DStream/ThirdParty/o3ds/
   git commit -m "Update O3DS core prebuilt libraries to $(git rev-parse --short HEAD)"
   git push
   ```

## What Gets Built

The O3DS core library includes:
- **NNG** (statically linked): Network messaging library
- **FlatBuffers** (headers only): Serialization framework
- **CRCpp** (headers only, compiled in): CRC calculation
- **O3DS core sources**: All core O3DS functionality

The resulting `o3ds.lib` (Windows) or `libo3ds.a` (Linux/macOS) is a fully self-contained static library with all dependencies embedded except:
- Platform sockets (ws2_32.lib on Windows, pthread on Linux)
- Standard C/C++ libraries

## When to Rebuild

You should rebuild and update the O3DS core libraries when:
- Source files in `src/o3ds/` are modified
- The FlatBuffers schema (`src/o3ds.fbs`) changes
- The O3DS version is updated
- Dependencies (NNG, FlatBuffers) are updated
- Build configuration changes

## Verification

The release workflow includes a verification step that checks for the presence of required O3DS libraries before building the plugin for release. This ensures prebuilt libraries are present and prevents accidental releases without proper dependencies.

## CMake Configuration

The standalone build is configured in `Build/o3ds-core/CMakeLists.txt` and:
- Builds from the repository's `src/` directory
- Uses submodules for NNG and FlatBuffers
- Sets `O3DS_VERSION_TAG` at build time
- Produces platform-specific static libraries
- Installs headers maintaining directory structure

## Troubleshooting

### Workflow fails to find o3ds_generated.h
The FlatBuffers generated header must be checked into `src/o3ds_generated.h`. If missing, generate it locally:
```bash
flatc --cpp -o src src/o3ds.fbs
git add src/o3ds_generated.h
git commit -m "Add generated FlatBuffers header"
```

### Library linking errors in Unreal plugin
Ensure all three platform libraries are committed and present in:
- `plugins/unreal/Open3DStream/ThirdParty/o3ds/lib/Win64/Release/o3ds.lib`
- `plugins/unreal/Open3DStream/ThirdParty/o3ds/lib/Linux/Release/libo3ds.a`
- `plugins/unreal/Open3DStream/ThirdParty/o3ds/lib/Mac/Release/libo3ds.a`

### Submodule initialization issues
The workflow automatically initializes submodules with `submodules: 'recursive'`. If building locally, run:
```bash
git submodule update --init --recursive
```

## Related Documentation

- [Build libdatachannel workflow](.github/workflows/build-libdatachannel.yml) - Similar pattern for WebRTC dependencies
- [Unreal Plugin Release workflow](.github/workflows/unreal-plugin-release.yml) - Uses prebuilt libraries
- [O3DS ThirdParty README](plugins/unreal/Open3DStream/ThirdParty/o3ds/README.md) - Library directory documentation
