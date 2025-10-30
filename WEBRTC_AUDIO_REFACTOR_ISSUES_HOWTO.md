# How to Create WebRTC Audio Refactor Issues

This guide walks through creating the GitHub Issues for the WebRTC Audio Path Refactor.

## Overview

You have two options for creating the issues:
- **Option A (Recommended):** One-click via GitHub Actions workflow - easiest, runs in the cloud
- **Option B:** Local automation via Python script - requires local setup but fully controllable

Both options read from `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md` and create 8 issues (1 Epic + 7 sub-issues) with proper labels and cross-references.

---

## Option A: One-Click via GitHub Actions

### Prerequisites
- Write access to the `lifelike-and-believable/Open3DStream` repository
- Access to the GitHub Actions tab

### Steps

1. **Navigate to Actions:**
   - Go to https://github.com/lifelike-and-believable/Open3DStream/actions
   - Find the workflow named **"Create WebRTC Audio Issues"**

2. **Trigger the workflow:**
   - Click on the workflow name
   - Click the **"Run workflow"** dropdown (top right)
   - Optionally customize the Epic title (default: "[EPIC] WebRTC Audio Path Refactor")
   - Click **"Run workflow"** button

3. **Monitor progress:**
   - The workflow will appear in the runs list
   - Click on the run to see detailed logs
   - Workflow creates the Epic first, captures its number, then creates Issues 1-7
   - Each sub-issue automatically references the Epic number

4. **Verify results:**
   - Once complete, go to https://github.com/lifelike-and-believable/Open3DStream/issues
   - You should see 8 new issues with proper labels
   - Epic issue contains checklist linking to all sub-issues

### Troubleshooting

**Workflow fails with authentication error:**
- Ensure GITHUB_TOKEN has proper permissions (should be automatic for workflow_dispatch)

**Issues are created but incomplete:**
- Check the workflow logs for parsing errors
- Verify `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md` is well-formed

**Need to re-run:**
- You can run the workflow multiple times (it will create duplicate issues, so be careful)
- Consider closing duplicates if needed

---

## Option B: Local via gh CLI

### Prerequisites
- GitHub CLI (`gh`) installed: https://cli.github.com/
- Authenticated with GitHub: `gh auth login`
- Python 3.8+ installed
- Write access to the repository

### Setup

1. **Install and authenticate GitHub CLI:**
   ```bash
   # Install gh (if not already installed)
   # macOS: brew install gh
   # Linux: See https://github.com/cli/cli/blob/trunk/docs/install_linux.md
   # Windows: choco install gh or download from releases
   
   # Authenticate
   gh auth login
   
   # Verify
   gh auth status
   ```

2. **Navigate to repository:**
   ```bash
   cd /path/to/Open3DStream
   ```

### Create Issues Automatically

**Using the Python script (recommended):**

```bash
# Run the automation script
python3 scripts/create_webrtc_audio_issues.py

# The script will:
# 1. Parse WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md
# 2. Create the Epic issue and capture its number
# 3. Create Issues 1-7, each referencing the Epic
# 4. Print a summary of created issue numbers
```

**Script output example:**
```
Parsing issue specifications from WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md...
Found 8 issues (1 Epic + 7 sub-issues)

Creating Epic issue...
✓ Epic created: #91 - [EPIC] WebRTC Audio Path Refactor

Creating sub-issues...
✓ Issue 1 created: #92 - WebRTC Audio: Unmask EnableAudioSend failures in adapter
✓ Issue 2 created: #93 - WebRTC Audio: Make audio capture component a pure PCM source
✓ Issue 3 created: #94 - WebRTC Audio: Wire audio before transport start in BroadcastComponent
✓ Issue 4 created: #95 - WebRTC Audio: Enforce EnableAudioSend must be called before Start
✓ Issue 5 created: #96 - WebRTC Audio: Improve logging and status diagnostics
✓ Issue 6 created: #97 - WebRTC Audio: Implement comprehensive test matrix
✓ Issue 7 created: #98 - WebRTC Audio: Final cleanup and documentation

All issues created successfully!
Epic: #91
Sub-issues: #92, #93, #94, #95, #96, #97, #98
```

### Create Issues Manually

If you prefer manual control, here's how to extract and create each issue:

```bash
# Set repository
REPO="lifelike-and-believable/Open3DStream"

# Create Epic first (captures issue number)
EPIC_BODY=$(sed -n '/## Issue 8:/,/^---$/p' WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md | tail -n +2 | head -n -1)
EPIC_NUM=$(gh issue create --repo $REPO \
  --title "[EPIC] WebRTC Audio Path Refactor" \
  --label "epic,area:unreal,area:webrtc,audio" \
  --body "$EPIC_BODY" | grep -oP '#\K\d+')

echo "Epic created: #$EPIC_NUM"

# Create Issue 1
ISSUE1_BODY=$(sed -n '/## Issue 1:/,/## Issue 2:/p' WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md | head -n -1 | tail -n +2)
ISSUE1_BODY="Part of Epic: #${EPIC_NUM}

$ISSUE1_BODY"
gh issue create --repo $REPO \
  --title "WebRTC Audio: Unmask EnableAudioSend failures in adapter" \
  --label "area:unreal,area:webrtc,bug,audio" \
  --body "$ISSUE1_BODY"

# Repeat for Issues 2-7 with similar pattern...
# (See scripts/create_webrtc_audio_issues.py for full automation)
```

---

## Option C: Manual Creation via GitHub Web Interface

If you prefer to create issues manually through the web interface:

### Step 1: Create the Epic Issue

1. Go to https://github.com/lifelike-and-believable/Open3DStream/issues/new
2. **Title:** `[EPIC] WebRTC Audio Path Refactor`
3. **Labels:** Select: `epic`, `area:unreal`, `area:webrtc`, `audio`
4. **Body:** Open `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md`, find "## Issue 8: Epic", and copy everything from "### Title" down to the "---" separator
5. Click "Submit new issue"
6. **Note the issue number** (e.g., #91) - you'll reference this in sub-issues

### Step 2: Create Sub-Issues (Issues 1-7)

For each issue (1-7), repeat this process:

1. Go to https://github.com/lifelike-and-believable/Open3DStream/issues/new
2. Find the issue section in `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md`
3. **Title:** Copy from the "### Title" field in the issue spec
4. **Labels:** Copy from the "### Labels" field in the issue spec
5. **Body:** 
   - Start with: `Part of Epic: #XX` (where XX is the Epic number from Step 1)
   - Then copy everything from the issue's "### Priority" section down to the "---" separator
6. Click "Submit new issue"
7. Repeat for all 7 sub-issues

**Issue titles for reference:**
- Issue 1: `WebRTC Audio: Unmask EnableAudioSend failures in adapter`
- Issue 2: `WebRTC Audio: Make audio capture component a pure PCM source`
- Issue 3: `WebRTC Audio: Wire audio before transport start in BroadcastComponent`
- Issue 4: `WebRTC Audio: Enforce EnableAudioSend must be called before Start`
- Issue 5: `WebRTC Audio: Improve logging and status diagnostics`
- Issue 6: `WebRTC Audio: Implement comprehensive test matrix`
- Issue 7: `WebRTC Audio: Final cleanup and documentation`

### Step 3: Update Epic with Sub-Issue Links

After creating all 7 sub-issues:
1. Edit the Epic issue
2. Replace the placeholder checkboxes in the "Sub-Issues" section with actual issue numbers
3. Save

---

## Issue Content Location

All detailed issue content is available in:
- **Detailed specs:** `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md` (8 issues with full details)
- **Summary:** `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md` (executive overview)
- **Automation:** `scripts/create_webrtc_audio_issues.py` (Python script)
- **Workflow:** `.github/workflows/create-webrtc-audio-issues.yml` (GitHub Actions)

## Recommended Labels

The following labels will be applied automatically:
- `epic` - For the tracking issue (Issue 8)
- `area:unreal` - Unreal Engine specific work (all issues)
- `area:webrtc` - WebRTC functionality (all issues)
- `audio` - Audio-related features (all issues)
- `bug` - Issue #1 (fixing masked failures)
- `refactor` - Issues #2, #3, #4 (architectural changes)
- `enhancement` - Issue #5 (diagnostics improvement)
- `testing` - Issue #6 (test creation)
- `documentation` - Issue #7 (docs and cleanup)

These labels should exist in the repository. If any are missing, they can be created on first use.

## Issue Dependencies

Dependencies are documented in each issue and will be automatically included in the issue bodies:

- **Issue 3** depends on **Issue 2** (needs new AudioCaptureComponent API)
- **Issue 5** depends on **Issues 1-4** (diagnostics for refactored system)
- **Issue 6** depends on **Issues 1-5** (testing complete refactor)
- **Issue 7** depends on **all previous issues** (final cleanup)

## Verification Checklist

After creating issues (via any method):
- [ ] Epic issue created with `epic,area:unreal,area:webrtc,audio` labels
- [ ] All 7 sub-issues created with appropriate labels
- [ ] Each sub-issue body starts with "Part of Epic: #XX"
- [ ] Epic issue body contains checklist with all sub-issue links
- [ ] Dependencies are noted in issue descriptions
- [ ] All issue numbers recorded for tracking

---

## Troubleshooting

### GitHub CLI Issues

**"gh: command not found":**
- Install GitHub CLI: https://cli.github.com/

**"authentication required":**
```bash
gh auth login
# Follow the prompts to authenticate
```

**"resource not accessible by personal access token":**
- Ensure your token has `repo` scope
- Re-authenticate: `gh auth login --web`

### Python Script Issues

**"No module named 'X'":**
```bash
# The script uses only stdlib, no extra packages needed
python3 --version  # Ensure Python 3.8+
```

**"Permission denied":**
```bash
chmod +x scripts/create_webrtc_audio_issues.py
```

### Parsing Issues

**"Could not find Issue X in spec file":**
- Verify `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md` exists
- Check that issue headers follow format: `## Issue X: Title`

**"Malformed issue specification":**
- Ensure each issue section ends with `---`
- Check for consistent markdown formatting

### GitHub Actions Issues

**"Workflow not found":**
- Verify `.github/workflows/create-webrtc-audio-issues.yml` exists
- Check workflow is on the default branch
- Refresh the Actions tab

**"Issues created but Epic not linked":**
- Check workflow logs for Epic number capture
- Sub-issues should show "Part of Epic: #XX" in their bodies

---

## Support

For questions or issues:

**About the refactor:**
- Original plan: `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`
- Executive summary: `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md`

**About automation:**
- Python script: `scripts/create_webrtc_audio_issues.py --help`
- Workflow file: `.github/workflows/create-webrtc-audio-issues.yml`

**About GitHub CLI:**
- Documentation: https://cli.github.com/manual/
- Check status: `gh auth status`
