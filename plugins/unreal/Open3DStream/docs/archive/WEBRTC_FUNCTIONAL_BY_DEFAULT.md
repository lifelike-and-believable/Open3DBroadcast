# Making WebRTC Functional by Default - Implementation Plan

## Current State Analysis

### What's Currently Done ✅
1. **UI Layer**: WebRTC options appear in LiveLink Source dialog (when O3DS_ENABLE_WEBRTC is defined)
2. **Backend Code**: WebRTC connector implementation exists (`webrtc_connector.cpp/h`)
3. **Build Configuration**: Unreal plugin defines `O3DS_ENABLE_WEBRTC` by default
4. **Conditional Linking**: Plugin gracefully handles missing WebRTC libraries

### What's Missing ❌
1. **Library Dependency**: `libdatachannel` is NOT built or included in thirdparty
2. **Build System Integration**: No automatic build of libdatachannel during thirdparty build
3. **CMake Default**: `O3DS_ENABLE_WEBRTC` defaults to `OFF` in src/CMakeLists.txt
4. **Library Distribution**: No WebRTC .lib files copied to Unreal plugin lib directory

## What Needs to Happen for Full Functionality

### Option 1: Add libdatachannel to Third-Party Dependencies (RECOMMENDED)

This makes WebRTC work out-of-the-box with no manual installation required.

#### Steps Required:

1. **Add libdatachannel as a Git Submodule**
   ```bash
   cd /workspaces/Open3DStream/thirdparty
   git submodule add https://github.com/paullouisageneau/libdatachannel.git
   ```

2. **Update thirdparty/CMakeLists.txt**
   ```cmake
   add_subdirectory(flatbuffers)
   add_subdirectory(nng)
   add_subdirectory(crccpp)
   
   # WebRTC Support - libdatachannel
   option(O3DS_BUILD_WEBRTC "Build libdatachannel for WebRTC support" ON)
   if(O3DS_BUILD_WEBRTC)
       set(NO_WEBSOCKET OFF CACHE BOOL "")
       set(NO_EXAMPLES ON CACHE BOOL "")
       set(NO_TESTS ON CACHE BOOL "")
       add_subdirectory(libdatachannel)
   endif()
   ```

3. **Update src/CMakeLists.txt - Enable by Default**
   ```cmake
   # Enable WebRTC support by default
   option(O3DS_ENABLE_WEBRTC "Enable WebRTC support" ON)
   if(O3DS_ENABLE_WEBRTC)
       find_package(LibDataChannel REQUIRED HINTS ${CMAKE_CURRENT_LIST_DIR}/../usr)
       add_compile_definitions(O3DS_ENABLE_WEBRTC)
   endif()
   ```

4. **Update push_lib.bat to Copy WebRTC Libraries**
   ```bat
   COPY "%BLD%\thirdparty\libdatachannel\RelWithDebInfo\datachannel.lib" "%DST%"\lib
   COPY "%BLD%\thirdparty\libdatachannel\RelWithDebInfo\datachannel.pdb" "%DST%"\lib
   
   REM Copy libdatachannel headers
   XCOPY /S /I /Y "%~DP0..\..\..\thirdparty\libdatachannel\include" "%DST%\lib\include\rtc\"
   ```

5. **Update Unreal Build.cs for Additional WebRTC Dependencies**
   ```csharp
   // libdatachannel also needs these Windows libraries
   if (File.Exists(DataChannelLib))
   {
       PublicAdditionalLibraries.Add(DataChannelLib);
       
       // Add Windows dependencies for libdatachannel
       if (Target.Platform == UnrealTargetPlatform.Win64)
       {
           PublicSystemLibraries.Add("ws2_32.lib");
           PublicSystemLibraries.Add("bcrypt.lib");
           PublicSystemLibraries.Add("secur32.lib");
           PublicSystemLibraries.Add("iphlpapi.lib");
       }
   }
   ```

#### Estimated Effort: **4-6 hours**
- Add submodule: 15 mins
- Update CMake files: 1 hour
- Test build process: 2 hours
- Update deployment scripts: 1 hour
- Documentation: 1 hour
- Testing: 1-2 hours

---

### Option 2: Use vcpkg for Dependency Management (ALTERNATIVE)

Use vcpkg to manage libdatachannel as an external dependency.

#### Steps Required:

1. **Add vcpkg.json Manifest**
   ```json
   {
     "name": "open3dstream",
     "version": "1.0.0",
     "dependencies": [
       "libdatachannel"
     ]
   }
   ```

2. **Update Build Scripts to Use vcpkg**
   ```bat
   IF NOT EXIST vcpkg (
       git clone https://github.com/microsoft/vcpkg.git
       cd vcpkg
       bootstrap-vcpkg.bat
       cd ..
   )
   
   vcpkg install libdatachannel --triplet x64-windows
   ```

3. **Update CMake to Use vcpkg Toolchain**
   ```bat
   cmake -H.. -B. -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake
   ```

#### Pros:
- ✅ Easier updates (vcpkg handles it)
- ✅ Less repository bloat

#### Cons:
- ❌ Requires internet connection for builds
- ❌ Additional vcpkg setup step
- ❌ Less control over libdatachannel version

#### Estimated Effort: **2-3 hours**

---

### Option 3: Provide Pre-built Binaries (QUICK BUT NOT IDEAL)

Distribute pre-built libdatachannel binaries in the repository.

#### Steps Required:

1. **Build libdatachannel for Target Platforms**
   - Windows x64
   - Linux x64
   - macOS (if needed)

2. **Add Binaries to Repository**
   ```
   thirdparty/
     libdatachannel/
       windows-x64/
         datachannel.lib
         datachannel.dll (if needed)
       linux-x64/
         libdatachannel.a
       include/
         rtc/...
   ```

3. **Update CMake to Use Pre-built Binaries**
   ```cmake
   if(O3DS_ENABLE_WEBRTC)
       set(LibDataChannel_DIR "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/libdatachannel/${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
       # Set up imported target
   endif()
   ```

#### Pros:
- ✅ Fastest to implement
- ✅ No build time for libdatachannel
- ✅ Works offline

#### Cons:
- ❌ Large binary files in repo
- ❌ Manual updates required
- ❌ Platform-specific builds needed
- ❌ Not ideal for open source

#### Estimated Effort: **2-3 hours**

---

## Recommended Approach

**Option 1: Add libdatachannel to thirdparty as a submodule** is the BEST approach because:

1. ✅ **Consistent with existing pattern** (nng, flatbuffers are also submodules)
2. ✅ **Full control** over version and build configuration
3. ✅ **Works offline** after initial clone
4. ✅ **Transparent build process** - everything builds from source
5. ✅ **Easy to maintain** and update
6. ✅ **Cross-platform** - works on Windows, Linux, macOS

## Implementation Checklist

To make WebRTC **fully functional by default**, complete these tasks:

### Phase 1: Add Dependency (Critical Path)
- [ ] Add libdatachannel as git submodule to `thirdparty/`
- [ ] Update `thirdparty/CMakeLists.txt` to build libdatachannel
- [ ] Update `thirdparty/build.bat` for Windows builds
- [ ] Test thirdparty build completes successfully

### Phase 2: Enable by Default
- [ ] Change `option(O3DS_ENABLE_WEBRTC ... OFF)` to `ON` in `src/CMakeLists.txt`
- [ ] Update `find_package(LibDataChannel)` to look in usr directory
- [ ] Test library build with WebRTC enabled

### Phase 3: Update Deployment
- [ ] Update `push_lib.bat` to copy datachannel.lib
- [ ] Update `push_lib.bat` to copy libdatachannel headers
- [ ] Create Linux equivalent script if needed
- [ ] Test library deployment to Unreal plugin

### Phase 4: Unreal Plugin Dependencies
- [ ] Update `Open3DStream.Build.cs` to add Windows system libraries
- [ ] Add WebRTC include paths to PrivateIncludePaths
- [ ] Test Unreal plugin compilation
- [ ] Test Unreal plugin loading

### Phase 5: Documentation & Testing
- [ ] Update README with "WebRTC enabled by default" notice
- [ ] Update build documentation
- [ ] Test end-to-end: Build → Deploy → Run in Unreal
- [ ] Test with actual WebRTC connection
- [ ] Update CI/CD if applicable

### Phase 6: Optional Enhancements
- [ ] Add example signaling server to repository
- [ ] Add WebRTC quick-start guide
- [ ] Create video/animated GIF demo
- [ ] Add troubleshooting section

## Current Build Process Flow

```
build.bat
  ├── build_env.bat (sets VS version)
  ├── thirdparty/build.bat
  │   ├── Builds flatbuffers
  │   ├── Builds nng
  │   ├── Builds crccpp
  │   └── (TODO: Build libdatachannel)
  ├── cmake (configures Open3DStream)
  ├── devenv (builds Debug)
  ├── devenv (builds RelWithDebInfo)
  └── package.py
```

## Post-Implementation Build Process

```
build.bat
  ├── build_env.bat (sets VS version)
  ├── thirdparty/build.bat
  │   ├── Builds flatbuffers
  │   ├── Builds nng
  │   ├── Builds crccpp
  │   └── ✅ Builds libdatachannel (NEW)
  ├── cmake (configures Open3DStream with -DO3DS_ENABLE_WEBRTC=ON)
  ├── devenv (builds Debug with WebRTC)
  ├── devenv (builds RelWithDebInfo with WebRTC)
  └── package.py

push_lib.bat
  ├── Copies nng.lib
  ├── Copies flatbuffers.lib
  ├── Copies open3dstreamstatic.lib
  ├── ✅ Copies datachannel.lib (NEW)
  └── ✅ Copies WebRTC headers (NEW)

Unreal Plugin Build
  ├── Links nng.lib
  ├── Links flatbuffers.lib
  ├── Links open3dstreamstatic.lib
  ├── ✅ Links datachannel.lib (NEW)
  ├── ✅ Links Windows system libs (NEW)
  └── ✅ WebRTC options appear in UI (ALREADY DONE)
```

## Testing Checklist

After implementation, verify:

1. **Build Tests**
   - [ ] Clean build of thirdparty succeeds
   - [ ] Clean build of src succeeds with WebRTC
   - [ ] All libraries install to usr directory
   - [ ] push_lib.bat copies all WebRTC files

2. **Integration Tests**
   - [ ] Unreal plugin compiles without errors
   - [ ] Unreal plugin loads in editor
   - [ ] LiveLink Source dialog shows WebRTC options
   - [ ] Can select "WebRTC Client" protocol

3. **Functional Tests**
   - [ ] Start signaling server (example)
   - [ ] Connect to WebRTC from Unreal
   - [ ] Receive data over WebRTC connection
   - [ ] Animation data streams correctly

4. **Regression Tests**
   - [ ] TCP protocol still works
   - [ ] UDP protocol still works
   - [ ] NNG protocols still work
   - [ ] No new warnings or errors

## Estimated Total Time

- **Minimum (Option 1)**: 4-6 hours
- **With Testing & Documentation**: 8-10 hours
- **With CI/CD Updates**: 10-12 hours

## Files to Modify

1. `thirdparty/CMakeLists.txt` - Add libdatachannel
2. `thirdparty/build.bat` - Build libdatachannel on Windows
3. `src/CMakeLists.txt` - Enable WebRTC by default
4. `plugins/unreal/Open3DStream/push_lib.bat` - Deploy WebRTC libs
5. `plugins/unreal/Open3DStream/Source/Open3DStream/Open3DStream.Build.cs` - Add system libs
6. `README.md` - Update documentation
7. `.gitmodules` - Add submodule reference

## Summary

**Current State**: WebRTC is "enabled" in the UI but NOT functional because libdatachannel is missing.

**Goal State**: WebRTC is fully functional out-of-the-box after running build.bat.

**Key Insight**: The UI and backend code are ready. We just need to:
1. Add libdatachannel to the build process
2. Deploy it to the Unreal plugin
3. Link it properly

**Recommended Next Step**: Implement Option 1 (add libdatachannel as submodule) for a production-ready solution.
