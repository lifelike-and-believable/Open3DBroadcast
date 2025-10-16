# Gauntlet Test Configuration

This directory contains Gauntlet test configurations for comprehensive automated testing of the Open3DStream plugin.

## Overview

[Gauntlet](https://docs.unrealengine.com/5.4/en-US/gauntlet-automation-framework-in-unreal-engine/) is Unreal Engine's automation framework for running tests across multiple devices and configurations.

## Test Configurations

### Open3DStreamTests.json

Basic smoke tests for the plugin:
- Validates plugin loads correctly
- Runs all `Open3DStream.*` automation tests
- Executes in headless mode (NullRHI)
- Maximum duration: 1 hour

## Running Tests

### Using the Script (Recommended)

```powershell
.\Build\Scripts\Run-Gauntlet.ps1 `
  -UEPath "C:\Program Files\Epic Games\UE_5.4" `
  -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
  -GauntletConfigs @("Open3DStreamTests") `
  -OutputDir "$PWD\Artifacts\Gauntlet" `
  -NullRHI
```

### Using RunUAT Directly

```powershell
& "C:\Program Files\Epic Games\UE_5.4\Engine\Build\BatchFiles\RunUAT.bat" RunGauntlet `
  -Project="ProjectSandbox/ProjectSandbox.uproject" `
  -Config="Open3DStreamTests" `
  -ReportDir="Artifacts/Gauntlet" `
  -NullRHI
```

## Test Reports

Gauntlet generates comprehensive reports:

```
Artifacts/Gauntlet/
├── Index.html              # Main report page
├── TestReport.json         # Machine-readable results
├── Logs/                   # Detailed logs
└── Screenshots/            # Any captured screenshots
```

## CI/CD Integration

The nightly workflow automatically runs Gauntlet tests:

```yaml
# .github/workflows/unreal-plugin-nightly.yml
- name: Run Gauntlet Tests
  run: |
    .\Build\Scripts\Run-Gauntlet.ps1 `
      -UEPath "$env:UE_ROOT" `
      -ProjectFile "$PWD\ProjectSandbox\ProjectSandbox.uproject" `
      -GauntletConfigs @("Open3DStreamTests") `
      -NullRHI
```

## Creating Custom Tests

### 1. Add Test Configuration

Create a new JSON file in this directory:

```json
{
  "ProjectName": "ProjectSandbox",
  "ProjectPath": "ProjectSandbox/ProjectSandbox.uproject",
  "TestName": "MyCustomTest",
  "RunTests": [
    "Open3DStream.NetworkTests",
    "Open3DStream.PerformanceTests"
  ]
}
```

### 2. Implement Tests in Plugin

Add automation tests to the plugin:

```cpp
// In plugin source
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FOpen3DStreamNetworkTest,
    "Open3DStream.NetworkTests.BasicConnection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FOpen3DStreamNetworkTest::RunTest(const FString& Parameters)
{
    // Test implementation
    TestTrue(TEXT("Connection succeeds"), /* condition */);
    return true;
}
```

### 3. Run Your Test

```powershell
.\Build\Scripts\Run-Gauntlet.ps1 `
  -GauntletConfigs @("MyCustomTest") `
  ...
```

## Configuration Options

### Test Flags

Common flags in test configurations:

- `-unattended` - Run without user interaction
- `-nullrhi` - Use null rendering hardware interface (headless)
- `-nosplash` - Skip splash screen
- `-nop4` - Disable Perforce integration
- `-log` - Enable logging
- `-verbose` - Verbose output

### Test Filters

Filter which tests to run:

```json
"RunTests": [
  "Open3DStream.*",                    // All Open3DStream tests
  "Open3DStream.NetworkTests.*",       // All network tests
  "Open3DStream.PerformanceTests.Benchmark"  // Specific test
]
```

## Best Practices

### Test Organization

Organize tests by category:

```
Open3DStream.
├── Core.               # Core functionality
├── NetworkTests.       # Network/streaming tests
├── PerformanceTests.   # Performance benchmarks
└── IntegrationTests.   # End-to-end tests
```

### Test Naming

Use descriptive names:
- ✅ `Open3DStream.NetworkTests.TCPConnection`
- ✅ `Open3DStream.Core.SubjectSerialization`
- ❌ `Open3DStream.Test1`

### Test Duration

Keep tests reasonable:
- **Unit tests**: < 1 second each
- **Integration tests**: < 10 seconds each
- **Full suite**: < 5 minutes
- **Performance tests**: < 30 minutes

### CI/CD Strategy

- **PR/CI**: Run fast smoke tests only
- **Nightly**: Run full test suite
- **Pre-release**: Run extended tests + benchmarks

## Troubleshooting

### Tests Not Found

If Gauntlet can't find tests:
1. Ensure plugin is linked: `.\Build\Scripts\Link-PluginIntoSandbox.ps1`
2. Verify test names match in code and config
3. Check plugin is enabled in ProjectSandbox.uproject

### Test Failures

For debugging test failures:
1. Check `Artifacts/Gauntlet/Logs/` for detailed output
2. Run tests locally without NullRHI for visual feedback
3. Add verbose logging to test code

### Timeout Issues

If tests timeout:
1. Increase `MaxDuration` in config JSON
2. Optimize slow tests
3. Split into multiple test suites

## Resources

- [Gauntlet Framework Documentation](https://docs.unrealengine.com/5.4/en-US/gauntlet-automation-framework-in-unreal-engine/)
- [Automation Testing Guide](https://docs.unrealengine.com/5.4/en-US/automation-system-overview-in-unreal-engine/)
- [Test Naming Conventions](https://docs.unrealengine.com/5.4/en-US/automation-technical-guide-in-unreal-engine/)
