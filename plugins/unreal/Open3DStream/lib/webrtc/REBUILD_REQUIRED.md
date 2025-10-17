# REBUILD REQUIRED - Missing Dependency Libraries

## Issue
The currently vendored `datachannel.lib` requires additional static library dependencies that are missing:
- `usrsctp.lib` (SCTP transport layer)
- `juice-static.lib` (ICE/STUN/TURN library)
- `mbedtls.lib`, `mbedx509.lib`, `mbedcrypto.lib` (TLS/DTLS library)

These dependencies are built by libdatachannel's CMake but were not collected in the original build artifacts.

## Linker Errors
Current CI build fails with 113 unresolved external symbols:
- `usrsctp_*` functions (SCTP operations)
- `juice_*` functions (ICE operations)
- `mbedtls_*` functions (TLS/crypto operations)
- `__std_*` functions (MSVC STL intrinsics)
- `_Thrd_*`, `_Cnd_*` functions (C11 threading)

## Solution
The `.github/workflows/build-libdatachannel.yml` workflow has been updated to collect all dependency libraries. To fix:

1. **Manual Trigger Build**: Go to GitHub Actions → "Build libdatachannel" → "Run workflow"
2. **Download Artifacts**: Download the `libdatachannel-windows` artifact
3. **Replace Libraries**: Extract and replace all files in this directory:
   ```
   plugins/unreal/Open3DStream/lib/webrtc/
   ├── datachannel.lib
   ├── usrsctp.lib  ← NEW
   ├── juice-static.lib  ← NEW
   ├── mbedtls.lib  ← NEW
   ├── mbedx509.lib  ← NEW
   ├── mbedcrypto.lib  ← NEW
   └── include/rtc/... (headers)
   ```
4. **Commit**: Commit the new `.lib` files to the repository
5. **CI**: CI build should now succeed with all dependencies linked

## Build.cs Changes
`Open3DStream.Build.cs` has been updated to link all required libraries:
```csharp
PublicAdditionalLibraries.Add(WebRTCDir + "datachannel.lib");
PublicAdditionalLibraries.Add(WebRTCDir + "usrsctp.lib");
PublicAdditionalLibraries.Add(WebRTCDir + "juice-static.lib");
PublicAdditionalLibraries.Add(WebRTCDir + "mbedtls.lib");
PublicAdditionalLibraries.Add(WebRTCDir + "mbedx509.lib");
PublicAdditionalLibraries.Add(WebRTCDir + "mbedcrypto.lib");
PublicSystemLibraries.AddRange(new string[] {
    "synchronization.lib",  // C11 threading functions
    "legacy_stdio_definitions.lib"  // MSVC STL intrinsics
});
```

## Why This Happened
When building libdatachannel with `-DBUILD_SHARED_LIBS=OFF`, CMake creates `datachannel.lib` as a static library that **depends on** other static libraries (usrsctp, juice, mbedtls). These dependencies must be linked by the final executable/DLL. The original workflow only copied `datachannel.lib`, missing its dependencies.

## Alternative: Combined Library
In theory, libdatachannel could be built with all dependencies combined into a single .lib file using CMake's `OBJECT` libraries or archiving. However, the current libdatachannel CMake configuration builds separate libraries, which is the standard approach.

## Verification
After collecting new libraries, verify with:
```powershell
# Check library sizes (datachannel should be ~24MB, others smaller)
Get-ChildItem *.lib | ForEach-Object { "$($_.Name): $([math]::Round($_.Length/1MB, 1))MB" }
```

Expected sizes:
- datachannel.lib: ~24MB
- usrsctp.lib: ~200-500KB
- juice-static.lib: ~100-300KB
- mbedtls.lib: ~1-2MB
- mbedx509.lib: ~200-500KB
- mbedcrypto.lib: ~2-4MB

## Status
- ✅ Workflow updated to collect all dependencies
- ❌ New libraries not yet built/committed
- ✅ Build.cs updated to link all dependencies
- ❌ CI still failing (waiting for library rebuild)
