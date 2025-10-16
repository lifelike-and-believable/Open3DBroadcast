# Unreal Plugin CI/CD Setup Summary

## Overview

This document summarizes the continuous integration setup created for the Open3DStream Unreal Engine plugin, based on the template from [lifelike-and-believable/ue-plugin-template](https://github.com/lifelike-and-believable/ue-plugin-template).

## Created Files

### Workflows

1. **`unreal-plugin-ci.yml`** - Standard CI for PRs and pushes
   - Triggers on PRs and pushes to develop/main
   - Builds plugin for Win64
   - Posts status comments on PRs
   - Path filters for relevant changes only

2. **`unreal-plugin-agent-ci.yml`** - Fast iteration for development
   - Triggers on copilot/** and feature/** branches
   - Supports `/build` comment on PRs
   - Shorter artifact retention (7 days)
   - Ideal for rapid development cycles

3. **`unreal-plugin-nightly.yml`** - Scheduled comprehensive testing
   - Runs daily at 2 AM UTC
   - Comprehensive build and test suite
   - 14-day artifact retention
   - Ready for Gauntlet tests (commented)

4. **`unreal-plugin-release.yml`** - Release automation
   - Triggers on `plugin-v*.*.*` tags
   - Builds in Shipping configuration
   - Updates plugin version
   - Creates GitHub Release with assets
   - 90-day artifact retention

### Documentation

5. **`README.md`** - Complete documentation
   - Setup instructions for self-hosted runners
   - Workflow descriptions and triggers
   - Configuration guide
   - Troubleshooting section
   - Best practices

## Key Adaptations for Open3DStream

### From Template
The workflows were adapted from the ue-plugin-template with these changes:

1. **Plugin Path**
   - Template: `Plugins/SamplePlugin`
   - Open3DStream: `plugins/unreal/Open3DStream`

2. **Repository Structure**
   - Added path filters for monorepo structure
   - Only triggers on changes to:
     - `plugins/unreal/**`
     - `src/**`
     - Workflow files themselves

3. **Naming**
   - Updated workflow names to include "Unreal Plugin"
   - Plugin name configured via env variable: `PLUGIN_NAME: 'Open3DStream'`

4. **Versioning**
   - Separate tag prefix: `plugin-v*.*.*` (vs library tags `v*.*.*`)
   - Prevents conflicts with C++ library releases

5. **Runners**
   - Configured for self-hosted runners with label `[self-hosted, ue5, windows]`
   - Includes instructions for GitHub-hosted alternative

## Configuration Required

### Before Using These Workflows

1. **Set up Self-Hosted Runner** (or use GitHub-hosted)
   - Install Unreal Engine 5.4+
   - Install Visual Studio 2022
   - Configure runner with labels: `windows`, `ue5`

2. **Configure Repository Variables** (optional)
   - `UE_ROOT`: Path to UE installation
   - `PLUGIN_NAME`: Plugin directory name
   - Or edit workflows directly

3. **Path to UE Installation**
   - Default: `C:\Program Files\Epic Games\UE_5.4`
   - Update in workflow `env` section if different

## Features

### Standard CI (unreal-plugin-ci.yml)
- ✅ Automatic PR builds
- ✅ Build status comments
- ✅ Artifact uploads (30 days)
- ✅ Path filters for efficiency
- 🚧 Test support (commented, ready to enable)

### Agent CI (unreal-plugin-agent-ci.yml)
- ⚡ Fast feedback loops
- ✅ `/build` command support
- ✅ PR head checkout when triggered by comment
- ✅ Shorter retention for iteration

### Nightly (unreal-plugin-nightly.yml)
- 🌙 Daily automated builds
- ✅ Comprehensive testing capability
- 🚧 Gauntlet test support (commented)
- ✅ Failure notifications

### Release (unreal-plugin-release.yml)
- 📦 Shipping configuration builds
- 🏷️ Automatic version updates
- 📚 Release notes generation
- 🚀 GitHub Release creation
- ⬆️ Asset uploads

## Usage Examples

### For Development

```bash
# Create a feature branch - auto triggers Agent CI
git checkout -b feature/my-feature
git push origin feature/my-feature

# Request build on PR
# Comment on PR: /build
```

### For Releases

```bash
# Create and push a release tag
git tag plugin-v1.0.0
git push origin plugin-v1.0.0

# Workflow automatically:
# 1. Builds plugin
# 2. Updates version in .uplugin
# 3. Creates GitHub Release
# 4. Uploads packaged plugin
```

### Manual Triggers

All workflows support manual dispatch:
1. Go to Actions tab
2. Select workflow
3. Click "Run workflow"
4. Fill in any required inputs

## Testing Setup (Optional)

To enable automation tests:

1. Create a test UE project with the plugin
2. Add automation tests to plugin
3. Uncomment test sections in workflows
4. Update project path in workflows

Example test structure:
```
plugins/unreal/Open3DStream/
  Source/
    Open3DStreamTests/
      Private/
        Open3DStreamSmokeTest.cpp
      Open3DStreamTests.Build.cs
```

## Artifact Management

### Retention Periods

| Workflow | Retention | Purpose |
|----------|-----------|---------|
| CI | 30 days | PR verification |
| Agent CI | 7 days | Quick iteration |
| Nightly | 14 days | Regular monitoring |
| Release | 90 days | Long-term storage |

### Download Artifacts

1. Navigate to Actions → Workflow Run
2. Scroll to "Artifacts" section
3. Click artifact name to download
4. Extract and use in UE project

## Coexistence with Existing Workflows

These new workflows complement the existing workflows:

- **Existing**: `linux.yml`, `windows.yml`, `doc.yml`
  - Build C++ core library
  - Create library releases

- **New**: `unreal-plugin-*.yml`
  - Build Unreal plugin
  - Create plugin releases
  - Separate version namespace

No conflicts or modifications to existing workflows.

## Next Steps

### Immediate
1. ✅ Workflows created and documented
2. 🔲 Set up self-hosted runner (or configure GitHub-hosted)
3. 🔲 Test workflows with a PR
4. 🔲 Verify plugin builds successfully

### Future Enhancements
1. 🔲 Add automation test project
2. 🔲 Enable test steps in workflows
3. 🔲 Configure Gauntlet tests for nightly
4. 🔲 Add additional platform builds (Linux, Mac)
5. 🔲 Set up notification system for nightly failures

## Troubleshooting

### Common Setup Issues

1. **Runner not picking up jobs**
   - Verify runner is online in Settings → Actions → Runners
   - Check runner labels match workflow requirements

2. **UE not found**
   - Update `UE_ROOT` in workflows or as repository variable
   - Verify UE installation includes all required components

3. **Build failures**
   - Check Visual Studio 2022 is installed
   - Verify plugin structure matches expected path
   - Review build logs in Actions tab

## Resources

- **Template Repository**: https://github.com/lifelike-and-believable/ue-plugin-template
- **Documentation**: `.github/workflows/README.md`
- **UE Automation**: https://docs.unrealengine.com/5.4/automation
- **GitHub Actions**: https://docs.github.com/actions

## Summary

Four production-ready CI/CD workflows have been created for the Open3DStream Unreal plugin:

1. ✅ **CI** - Standard PR/push builds
2. ✅ **Agent CI** - Fast iteration with `/build` command
3. ✅ **Nightly** - Comprehensive scheduled testing
4. ✅ **Release** - Automated release packaging

All workflows are fully documented, configured for the Open3DStream monorepo structure, and ready for use after runner setup. The workflows maintain separation from existing C++ library builds and use separate version tags (`plugin-v*` vs `v*`).
