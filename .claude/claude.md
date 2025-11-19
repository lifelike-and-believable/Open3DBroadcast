# Claude Code Project Rules

## CRITICAL RULES - ALWAYS FOLLOW

### 1. Always Use UE 5.7 for Building
**RULE:** Build with Unreal Engine 5.7 ONLY. Never use 5.4 or any other version.

**How to enforce:**
- Project file is already configured: `.uproject` has `"EngineAssociation": "5.7"`
- When building, use: 
"C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" ProjectSandboxEditor Win64 Development -Project="e:\OtherProjects\Open3DStream\ProjectSandbox\ProjectSandbox.uproject"

**Target.cs files must have:**
```cpp
DefaultBuildSettings = BuildSettingsVersion.V6;
IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
```

**Files that control this:**
- `ProjectSandbox/Source/ProjectSandbox.Target.cs`
- `ProjectSandbox/Source/ProjectSandboxEditor.Target.cs`

If either file is missing these settings, add them immediately.

---

### 2. NEVER GUESS APIs - Always Read Header Files First
**RULE:** Never assume API signatures, member names, or class structures. ALWAYS read the actual header files.

**Why:** Incorrect API usage causes compilation failures. Always check the source.

**Critical header files for this project:**
1. **Open3DStream Data APIs**
   - File: `ProjectSandbox/Plugins/Open3DBroadcast/ThirdParty/open3dstream/include/o3ds/model.h`
   - Classes: `Subject`, `SubjectList`, `Transform`, `TransformList`
   - **NEVER assume method names or member names**

2. **LiveKit FFI APIs**
   - Header File: `ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/include/livekit_ffi.h`
   - **Source Code Reference:** `E:\OtherProjects\livekit-ffi-ue\livekit_ffi` (for detailed implementation and undocumented features)
   - Functions: `lk_audio_track_create()`, `lk_audio_track_publish_pcm_i16()`, `lk_send_data_ex()`, etc.
   - **Check exact parameter types and order**
   - **For missing APIs or ambiguous functionality:** Refer to the source code in the livekit-ffi-ue repository
   - **To request new C++ wrapper APIs:** Ask and forward the request to the LiveKit FFI developers

**Example of WRONG approach:**
```cpp
// WRONG - guessing without checking header
for (int i = 0; i < List.size(); i++)
{
    const auto& item = List.at(i);        // Does .at() exist?
    NewList.add();                         // Does .add() exist?
    NewList.mutable_items(0) = item;      // Does mutable_items() exist?
}
```

**Example of CORRECT approach:**
```cpp
// CORRECT - read model.h first, then use actual API
// From model.h: SubjectList has public std::vector<Subject*> mItems
// From model.h: SubjectList has size_t size() method
// From model.h: SubjectList has operator[] for indexing

for (size_t i = 0; i < List.mItems.size(); ++i)
{
    O3DS::Subject* Subject = List.mItems[i];
    // ... use actual public members and methods confirmed in header
}
```

**When implementing:**
1. Open the header file FIRST
2. Read the class declaration completely
3. Note all public members and methods
4. Only then write code using those APIs
5. Build and verify

---

### 3. WebRTC Transport Implementation Standards

**Current Status:**
- ✅ Phase 1: Per-subject labeled data channels (complete)
- ✅ Phase 2: Per-subject audio tracks (complete)
- ✅ Build: Fixed and passing with UE 5.7

**Key Components:**
- `WebRTCSender.h/cpp` - Sends mocap data and audio per subject
- `WebRTCReceiver.h/cpp` - Receives and routes per-subject data
- Audio track support via per-subject `LkAudioTrackHandle` map

**If modifying these files:**
1. Check `livekit_ffi.h` for all FFI function signatures
2. Check `model.h` for O3DS data structure APIs
3. Ensure thread-safety with proper mutexes
4. Test with multiple concurrent senders
5. Build with UE 5.7

---

### 4. MCP Server and Tool Preferences

**RULE:** Always use Exa MCP tools for web searches and code context lookups.

**Why:** Exa tools are pre-configured and don't require permission prompts. They provide better performance and integration.

**Tools to use:**
- **Web searches:** `mcp__plugin_exa-mcp-server_exa__web_search_exa`
  - Use for: Finding articles, documentation, best practices, research
  - Parameters: `query`, `numResults`, `type` (auto/fast/deep), `livecrawl` (fallback/preferred)

- **Code context:** `mcp__plugin_exa-mcp-server_exa__get_code_context_exa`
  - Use for: API examples, library documentation, code patterns
  - Parameters: `query`, `tokensNum` (1000-50000, default 5000)

- **Library documentation:** Context7 tools
  - `mcp__context7__resolve-library-id` - Resolve package names to library IDs
  - `mcp__context7__get-library-docs` - Get up-to-date docs for libraries

**Tools to AVOID:**
- ❌ Built-in `WebSearch` tool - causes permission prompt issues in dontAsk mode
- Use MCP Exa tools instead

**Example usage:**
```
mcp__plugin_exa-mcp-server_exa__web_search_exa with query "WebRTC performance optimization"
mcp__plugin_exa-mcp-server_exa__get_code_context_exa with query "LiveKit FFI audio track creation"
```

---

### 5. Build Error Resolution Process

**If build fails:**
1. Check that you're using UE 5.7 (not 5.4)
2. Verify Target.cs files have correct settings
3. If API errors: READ THE HEADER FILE
4. Compare your code against actual API declarations
5. Fix and rebuild

**Common issues:**
- Using non-existent methods: Read the header file
- Wrong parameter types: Check function signature in header
- Missing members: Check class declaration in header

---

## Project Structure

```
ProjectSandbox/
├── Source/
│   ├── ProjectSandbox.Target.cs        (Must have V6 + Unreal5_7)
│   └── ProjectSandboxEditor.Target.cs  (Must have V6 + Unreal5_7)
├── Plugins/Open3DBroadcast/
│   ├── Source/Open3DTransportWebRTC/
│   │   ├── Private/Sender/WebRTCSender.h/cpp
│   │   ├── Private/Receiver/WebRTCReceiver.h/cpp
│   │   └── ThirdParty/
│   │       ├── open3dstream/include/o3ds/model.h    (Data APIs)
│   │       └── livekit_ffi/include/livekit_ffi.h    (WebRTC APIs)
└── ProjectSandbox.uproject              ("EngineAssociation": "5.7")
```

---

## Recent History

**November 18, 2025:**
- Implemented Phase 1: Per-subject labeled data channels
- Implemented Phase 2: Per-subject audio tracks
- Fixed API usage errors by reading actual header files
- Fixed build configuration for UE 5.7
- Created development guidelines

**Lesson:** Always read header files before writing code. Don't guess APIs.

---

## Contact / Questions

If you need to work on WebRTC transport:
1. Read this claude.md file first
2. Read the relevant header files
3. Check DEVELOPMENT_GUIDELINES.md for detailed examples
4. Build with UE 5.7
5. Test thoroughly before committing

---

**Last Updated:** November 19, 2025
**Version:** 1.1
**Status:** Active - Follow these rules for all work on this project

## Changes in v1.1
- Added Rule 4: MCP Server and Tool Preferences (always use Exa MCP tools for searches)
