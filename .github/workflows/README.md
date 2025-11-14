# Unreal Plugin CI/CD Workflows

This directory contains GitHub Actions workflows for building, testing, and releasing Unreal Engine plugins in this repository.

## Overview

### Open3DStream Plugin Workflows

The CI/CD setup for the Open3DStream plugin consists of four main workflows:

1. **Unreal Plugin CI** (`unreal-plugin-ci.yml`) - Standard CI for PRs and pushes
2. **Unreal Plugin Agent CI** (`unreal-plugin-agent-ci.yml`) - Fast iteration for development branches
3. **Unreal Plugin Nightly** (`unreal-plugin-nightly.yml`) - Scheduled comprehensive testing
4. **Unreal Plugin Release** (`unreal-plugin-release.yml`) - Release builds and publishing

### Open3DBroadcast Plugin Workflows

The CI/CD setup for the self-contained Open3DBroadcast plugin consists of four simplified workflows:

1. **Open3DBroadcast Plugin CI** (`open3dbroadcast-plugin-ci.yml`) - Standard CI for PRs and pushes
2. **Open3DBroadcast Plugin Tests** (`open3dbroadcast-plugin-test.yml`) - Feature branch testing
3. **Open3DBroadcast Plugin Nightly** (`open3dbroadcast-plugin-nightly.yml`) - Scheduled nightly builds
4. **Open3DBroadcast Plugin Release** (`open3dbroadcast-plugin-release.yml`) - Release builds and publishing

**Key Difference**: Open3DBroadcast workflows are simpler as the plugin is self-contained with all third-party libraries pre-compiled and included in the plugin directory. No pre-build steps or external dependencies are required.

## Prerequisites

### Self-Hosted Runner Setup

These workflows are designed to run on self-hosted runners with Unreal Engine 5.4+ installed. To set up a runner:

1. **Install Unreal Engine 5.4+**
   - Download from Epic Games Launcher
   - Default path: `C:\Program Files\Epic Games\UE_5.4`

2. **Set up GitHub Actions Runner**
   ```powershell
   # Download and extract the runner
   mkdir actions-runner && cd actions-runner
   Invoke-WebRequest -Uri https://github.com/actions/runner/releases/download/v2.311.0/actions-runner-win-x64-2.311.0.zip -OutFile actions-runner-win-x64-2.311.0.zip
   Expand-Archive -Path actions-runner-win-x64-2.311.0.zip -DestinationPath .
   
   # Configure the runner
   .\config.cmd --url https://github.com/lifelike-and-believable/Open3DStream --token YOUR_TOKEN
   
   # Add labels during configuration: windows, ue5
   
   # Install and start the runner as a service
   .\svc.cmd install
   .\svc.cmd start
   ```

3. **Install Visual Studio 2022**
   - Required for building Unreal plugins
   - Install "Game Development with C++" workload

### Alternative: GitHub-Hosted Runners

To use GitHub-hosted runners:

1. Install UE on the runner during workflow execution (time-consuming)
2. Or use a custom Docker container with UE pre-installed
3. Update the `runs-on` field in each workflow to `windows-latest`

## Workflows

### 1. Unreal Plugin CI

**File:** `unreal-plugin-ci.yml`

**Triggers:**
- Pull requests to `develop` or `main` branches
- Pushes to `develop` or `main` branches
- Manual trigger via workflow dispatch

**What it does:**
- ✅ Builds the plugin for Win64
- ✅ Packages the plugin
- ✅ Uploads build artifacts
- ✅ Posts status comments on PRs
- 🚧 Optionally runs automation tests (when configured)

**Usage:**
```bash
# Automatically triggered on PR/push

# Manual trigger:
# Go to Actions → Unreal Plugin CI → Run workflow
```

### 2. Unreal Plugin Agent CI

**File:** `unreal-plugin-agent-ci.yml`

**Triggers:**
- Pushes to `copilot/**` or `feature/**` branches
- Manual trigger via workflow dispatch
- Comment `/build` on any PR

**What it does:**
- ⚡ Fast feedback for rapid iteration
- ✅ Builds the plugin for Win64
- ✅ Packages the plugin (7-day retention)
- ✅ Supports on-demand PR builds via comment

**Usage:**
```bash
# Triggered automatically on copilot/feature branches

# Trigger on any PR by commenting:
/build

# This will fetch the PR head and build it
```

### 3. Unreal Plugin Nightly

**File:** `unreal-plugin-nightly.yml`

**Triggers:**
- Scheduled: Daily at 2 AM UTC
- Manual trigger via workflow dispatch

**What it does:**
- 🌙 Runs comprehensive builds nightly
- ✅ Builds the plugin for Win64
- ✅ Uploads artifacts (14-day retention)
- 🚧 Optionally runs extensive test suites (when configured)
- 🚧 Optionally runs Gauntlet tests (when configured)

**Usage:**
```bash
# Automatically runs daily at 2 AM UTC

# Manual trigger:
# Go to Actions → Unreal Plugin Nightly → Run workflow
```

### 4. Unreal Plugin Release

**File:** `unreal-plugin-release.yml`

**Triggers:**
- Push tags matching `plugin-v*.*.*` (e.g., `plugin-v1.0.0`)
- Manual trigger via workflow dispatch

**What it does:**
- 📦 Builds plugin in Shipping configuration
- 🏷️ Updates plugin version in `.uplugin` file
- 📚 Generates release notes
- 🚀 Creates GitHub Release
- ⬆️ Uploads packaged plugin as release asset

**Usage:**
```bash
# Create and push a version tag:
git tag plugin-v1.0.0
git push origin plugin-v1.0.0

# Or manually trigger:
# Go to Actions → Unreal Plugin Release → Run workflow
# Enter version tag: plugin-v1.0.0
```

## Configuration

### Environment Variables

Each workflow uses these environment variables (configurable):

```yaml
env:
  UE_ROOT: 'C:\Program Files\Epic Games\UE_5.4'
  PLUGIN_NAME: 'Open3DStream'
```

**To customize:**

1. **Repository Variables** (recommended):
   - Go to Settings → Secrets and variables → Actions → Variables
   - Add `UE_ROOT` and `PLUGIN_NAME`
   - Workflows will use these automatically

2. **Edit workflows directly**:
   - Modify the `env` section in each workflow file

### Path Filters

The CI workflow has path filters to only run when relevant files change:

```yaml
paths:
  - 'plugins/unreal/**'
  - 'src/**'
  - '.github/workflows/unreal-plugin-ci.yml'
```

Adjust these if your project structure differs.

## Adding Automation Tests

All workflows have commented sections for automation tests. To enable:

1. **Create a test project**:
   - Set up a minimal UE project that includes the plugin
   - Add automation tests in the plugin's Tests module

2. **Uncomment test sections** in workflows

3. **Configure test paths**:
   ```yaml
   - name: Run Automation Tests
     run: |
       $editor = Join-Path $env:UE_ROOT 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
       $projectPath = "path\to\TestProject.uproject"  # Update this
       # ...
   ```

## Artifacts

### Artifact Retention

| Workflow | Artifact Name | Retention |
|----------|---------------|-----------|
| CI | `Open3DStream-Win64-{sha}` | 30 days |
| Agent CI | `Open3DStream-AgentCI-{sha}` | 7 days |
| Nightly | `Open3DStream-Nightly-Win64-{sha}` | 14 days |
| Release | `Open3DStream-Release-{version}` | 90 days |

### Downloading Artifacts

1. Go to Actions tab
2. Select the workflow run
3. Scroll to "Artifacts" section
4. Click to download

## Troubleshooting

### Common Issues

**1. UE not found**
```
Error: UE not found at: C:\Program Files\Epic Games\UE_5.4
```
**Solution:** Update `UE_ROOT` in workflow or repository variables

**2. Plugin file not found**
```
Error: Plugin file not found at: plugins\unreal\Open3DStream\Open3DStream.uplugin
```
**Solution:** Ensure `PLUGIN_NAME` matches your plugin directory name

**3. Build fails with "RunUAT not found"**
```
Error: RunUAT not found under C:\Program Files\Epic Games\UE_5.4
```
**Solution:** Verify UE installation is complete and includes Engine/Build directory

**4. Runner offline**
```
Waiting for a runner to pick up this job...
```
**Solution:** Check runner status in Settings → Actions → Runners

### Debug Mode

To enable verbose logging, add this to any workflow step:

```yaml
- name: Build plugin (Win64)
  env:
    RUNNER_DEBUG: 1
  run: |
    # Your commands here
```

## Best Practices

### For Contributors

1. **Use Agent CI for rapid iteration**
   - Push to `feature/*` branches
   - Comment `/build` on PRs for on-demand builds

2. **Keep PRs focused**
   - Path filters ensure only relevant changes trigger builds

3. **Check artifacts before merging**
   - Download and test the packaged plugin locally

### For Maintainers

1. **Use consistent version tags**
   - Format: `plugin-v{major}.{minor}.{patch}`
   - Example: `plugin-v1.2.3`

2. **Monitor nightly builds**
   - Set up notifications for failures
   - Address issues promptly

3. **Review release notes**
   - Auto-generated notes can be edited after creation

## Open3DBroadcast Plugin Workflows

The Open3DBroadcast plugin uses simplified workflows because it's fully self-contained:

### What's Different

**No Pre-Build Steps Required**:
- All third-party libraries are pre-compiled and included in the plugin
- No CMake builds or external downloads needed
- Faster workflow execution (5-10 minutes saved per run)

**Self-Contained Structure**:
```
ProjectSandbox/Plugins/Open3DBroadcast/
├── ThirdParty/
│   ├── open3dstream/
│   │   ├── include/
│   │   └── lib/Win64/open3dstreamstatic.lib
│   ├── flatbuffers/
│   │   ├── include/
│   │   └── lib/Win64/flatbuffers.lib
│   └── opus/
│       ├── include/
│       └── lib/Win64/opus.lib
└── Source/
    └── Open3DTransportNNG/
        └── ThirdParty/nng/
            └── lib/Win64/nng.lib
```

### Workflow Triggers

| Workflow | Triggers | Configuration |
|----------|----------|---------------|
| **CI** | PRs to `develop`/`main`, pushes to `develop`/`main` | Development |
| **Tests** | Pushes to `feature/*`, `bugfix/*`, `hotfix/*` | Development |
| **Nightly** | Daily at 3 AM UTC | Shipping |
| **Release** | Tags matching `open3dbroadcast-v*.*.*` | Shipping |

### Version Tags

For Open3DBroadcast releases, use the format:
```bash
git tag open3dbroadcast-v1.0.0
git push origin open3dbroadcast-v1.0.0
```

This will automatically:
1. Update the plugin descriptor version
2. Build in Shipping configuration
3. Create a GitHub release with installation instructions
4. Upload the packaged plugin as a release asset

### Path Filters

Open3DBroadcast workflows only trigger on changes to:
- `ProjectSandbox/Plugins/Open3DBroadcast/**`
- `ProjectSandbox/Source/**`
- `Build/**`
- `.github/workflows/open3dbroadcast-*.yml`

Changes to core library sources (`src/`, `thirdparty/`) do NOT trigger these workflows since the plugin is self-contained.

## Migration from Template

These workflows were adapted from [ue-plugin-template](https://github.com/lifelike-and-believable/ue-plugin-template) with the following changes:

- ✅ Adjusted plugin path to `plugins/unreal/Open3DStream`
- ✅ Added path filters for monorepo structure
- ✅ Updated plugin name references
- ✅ Configured for Open3DStream project structure
- ✅ Maintained compatibility with existing C++ library builds
- ✅ Created simplified Open3DBroadcast workflows for self-contained plugin

## Additional Resources

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Unreal Engine Automation](https://docs.unrealengine.com/5.4/en-US/automation-system-overview-in-unreal-engine/)
- [Self-Hosted Runners](https://docs.github.com/en/actions/hosting-your-own-runners)

## Support

For issues with the workflows:
- Check the [Troubleshooting](#troubleshooting) section
- Review workflow logs in the Actions tab
- Open an issue in the repository

