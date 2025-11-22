# Research and Resource Identification Checklist

Before creating a plan, conduct thorough research using this checklist.

## A. Codebase Analysis

### Identify Affected Components
- [ ] Use GitHub code search to find relevant files, classes, and functions
- [ ] Check `src/o3ds/` for core protocol and connector implementations
- [ ] Check `ProjectSandbox/Plugins/Open3DBroadcast` for Unreal Engine integration
- [ ] Review `test/` and `Tests/` for existing test infrastructure

### Understand Current Implementation
- [ ] Read existing code to understand patterns and conventions
- [ ] Identify interfaces and base classes that new code should follow
- [ ] Note any TODOs or FIXMEs related to the planned work

### Map Dependencies
- [ ] Identify which modules/classes depend on the code to be changed
- [ ] Use `grep` or code search to find all usages of APIs that will change
- [ ] Check `CMakeLists.txt` and `.uproject` files for build dependencies

## B. API and Documentation Research

### Verify Unreal Engine APIs
- [ ] For ANY Unreal Engine API usage, check UE 5.7 documentation first
- [ ] Use web search: "Unreal Engine C++ API Reference" + class name
- [ ] Access official Unreal Engine source (GitHub or local installation) when available
- [ ] Note: @lifelike-and-believable/UnrealEngine repository may be accessible via GitHub MCP Server
- [ ] NEVER assume API signatures - always verify

### Review Protocol Specifications
- [ ] Review `src/o3ds.fbs` - FlatBuffers schema (authoritative source)
- [ ] Understand serialization format and versioning requirements

### Check External Dependencies
- [ ] Review thirdparty libraries in `thirdparty/` directory
- [ ] Identify if new dependencies are needed (and their security implications)
- [ ] Verify compatibility with existing build systems

## C. Testing Infrastructure

### Identify Relevant Test Frameworks
- [ ] Unreal automation tests (`Build/Scripts/Run-AutomationTests.ps1`)
- [ ] Gauntlet tests (`Build/Scripts/Run-Gauntlet.ps1`)
- [ ] C++ unit tests (e.g., `test_curves.cpp`)

### Understand Test Requirements
- [ ] What test coverage is expected for this type of change
- [ ] Which existing tests might be affected
- [ ] What new test scenarios are needed

## D. CI/CD and Build Systems

### Review Build Workflows
- [ ] Check `.github/workflows/` for relevant CI pipelines
- [ ] Understand build requirements for different platforms (Windows, Linux)
- [ ] Identify which workflows will validate the changes

### Note Build Tools and Commands
- [ ] CMake build configuration
- [ ] Unreal Editor build commands
- [ ] Package/release processes if applicable
