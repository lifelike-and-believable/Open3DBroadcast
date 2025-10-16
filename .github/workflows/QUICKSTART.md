# Unreal Plugin CI/CD Quick Reference

## 🚀 Quick Start

### First Time Setup

1. **Configure UE Path** (if different from default)
   ```yaml
   # In each workflow file, update:
   env:
     UE_ROOT: 'C:\Program Files\Epic Games\UE_5.4'  # Change this
   ```

2. **Set up Self-Hosted Runner**
   - Requirements: Windows, UE 5.4+, VS 2022
   - Labels: `self-hosted`, `windows`, `ue5`
   - See main README for detailed setup

3. **Test the Workflows**
   - Create a test PR
   - Comment `/build` to trigger Agent CI
   - Verify artifacts are generated

## 📋 Workflow Cheat Sheet

| Workflow | Trigger | Use Case |
|----------|---------|----------|
| **unreal-plugin-ci** | PR/Push to develop/main | Standard PR verification |
| **unreal-plugin-agent-ci** | Push to feature/*, comment `/build` | Rapid development iteration |
| **unreal-plugin-nightly** | Daily 2 AM UTC | Comprehensive testing |
| **unreal-plugin-release** | Tag `plugin-v*.*.*` | Create plugin releases |

## 🎯 Common Tasks

### Build a PR
```bash
# Option 1: Push to PR branch (automatic)
git push origin my-branch

# Option 2: Comment on PR
/build
```

### Create a Release
```bash
# Tag format: plugin-v{major}.{minor}.{patch}
git tag plugin-v1.0.0
git push origin plugin-v1.0.0

# Workflow will automatically:
# ✓ Build in Shipping
# ✓ Update .uplugin version
# ✓ Create GitHub Release
# ✓ Upload packaged plugin
```

### Manually Trigger Build
1. Go to **Actions** tab
2. Select workflow
3. Click **Run workflow**
4. Select branch/enter inputs
5. Click **Run workflow**

### Download Artifacts
1. **Actions** → Select workflow run
2. Scroll to **Artifacts** section
3. Click artifact name to download

## ⚙️ Configuration

### Environment Variables
```yaml
env:
  UE_ROOT: 'C:\Program Files\Epic Games\UE_5.4'  # UE installation path
  PLUGIN_NAME: 'Open3DStream'                     # Plugin directory name
```

### Path Filters (CI only)
```yaml
paths:
  - 'plugins/unreal/**'      # Unreal plugin files
  - 'src/**'                 # Core library (plugin dependencies)
  - '.github/workflows/*.yml' # Workflow changes
```

### Artifact Retention
- **CI**: 30 days
- **Agent CI**: 7 days
- **Nightly**: 14 days
- **Release**: 90 days

## 🧪 Enable Testing (Optional)

In each workflow, uncomment these sections:

```yaml
# 1. Run tests
- name: Run Automation Tests (optional)
  # Uncomment this section
  
# 2. Upload results
- name: Upload test reports (optional)
  # Uncomment this section
```

Then update the project path:
```powershell
$projectPath = Join-Path $PWD "path\to\TestProject.uproject"
```

## 🔍 Troubleshooting

### Workflow Not Running?
- ✓ Check runner status: Settings → Actions → Runners
- ✓ Verify path filters if CI workflow
- ✓ Check branch name for Agent CI (feature/*, copilot/*)

### Build Failing?
- ✓ Verify UE_ROOT path in workflow
- ✓ Check Visual Studio 2022 is installed
- ✓ Review build logs in Actions tab
- ✓ Ensure plugin path: `plugins/unreal/Open3DStream/`

### Artifacts Not Appearing?
- ✓ Check if build completed successfully
- ✓ Wait a few seconds after job completion
- ✓ Verify retention period hasn't expired

## 📝 Version Tags

### Tag Format
```bash
plugin-v{major}.{minor}.{patch}

Examples:
- plugin-v1.0.0    # First release
- plugin-v1.1.0    # New features
- plugin-v1.1.1    # Bug fixes
```

### Why "plugin-v" prefix?
Separates plugin releases from C++ library releases:
- `plugin-v*.*.*` → Unreal plugin releases
- `v*.*.*` → C++ library releases (existing)

## 🎨 Workflow Customization

### Add Linux Build
```yaml
- name: Build plugin (Linux)
  run: |
    & "$uat" BuildPlugin `
      -Plugin="${{ steps.find.outputs.plugin_path }}" `
      -Package="$pkg" `
      -TargetPlatforms=Linux `
      -Rocket
```

### Change UE Version
```yaml
env:
  UE_ROOT: 'C:\Program Files\Epic Games\UE_5.5'  # Update version
```

### Add Slack Notifications
```yaml
- name: Notify Slack
  if: failure()
  uses: slackapi/slack-github-action@v1
  with:
    webhook-url: ${{ secrets.SLACK_WEBHOOK }}
```

## 📚 Full Documentation

For detailed information, see:
- **[README.md](README.md)** - Complete setup guide
- **[SETUP_SUMMARY.md](SETUP_SUMMARY.md)** - Implementation details

## 🆘 Support

- **Issues**: Open a GitHub issue
- **Logs**: Check Actions tab → Workflow run → Job logs
- **UE Docs**: https://docs.unrealengine.com/5.4/automation
- **GitHub Actions**: https://docs.github.com/actions
