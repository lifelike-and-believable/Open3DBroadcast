# How to Create WebRTC Audio Refactor Issues

This guide walks through creating the GitHub Issues for the WebRTC Audio Path Refactor.

## Prerequisites

- GitHub CLI (`gh`) installed and authenticated
- OR access to GitHub web interface
- Permission to create issues in the `lifelike-and-believable/Open3DStream` repository

## Quick Start (Using GitHub CLI)

If you have `gh` CLI installed, you can create all issues with these commands:

```bash
# Navigate to repository root
cd /home/runner/work/Open3DStream/Open3DStream

# Set repository
REPO="lifelike-and-believable/Open3DStream"

# Create Epic issue first (will get issue number, e.g., #91)
gh issue create --repo $REPO \
  --title "[EPIC] WebRTC Audio Path Refactor" \
  --label "epic,area:unreal,area:webrtc,audio" \
  --body-file <(sed -n '/## Issue 8/,/^---$/p' /tmp/webrtc_audio_refactor_issues.md | grep -v "^## Issue 8" | grep -v "^---$")

# Note the epic issue number (e.g., #91)
EPIC_NUM=91  # UPDATE THIS with actual number

# Create Issue 1
gh issue create --repo $REPO \
  --title "WebRTC Audio: Unmask EnableAudioSend failures in adapter" \
  --label "area:unreal,area:webrtc,bug,audio" \
  --body-file /tmp/issue1_body.md

# ... repeat for issues 2-7 using their respective content from /tmp/webrtc_audio_refactor_issues.md
```

## Manual Creation (GitHub Web Interface)

### Step 1: Create the Epic Issue

1. Go to https://github.com/lifelike-and-believable/Open3DStream/issues/new
2. **Title:** `[EPIC] WebRTC Audio Path Refactor`
3. **Labels:** Select: `epic`, `area:unreal`, `area:webrtc`, and create `audio` if needed
4. **Body:** Copy content from Issue 8 section in `/tmp/webrtc_audio_refactor_issues.md`
5. Click "Submit new issue"
6. **Note the issue number** (e.g., #91) - you'll reference this in sub-issues

### Step 2: Create Sub-Issues (Issues 1-7)

For each issue, follow this pattern:

#### Issue 1: Phase A.1 - Unmask EnableAudioSend Failures

**URL:** https://github.com/lifelike-and-believable/Open3DStream/issues/new

**Title:**
```
WebRTC Audio: Unmask EnableAudioSend failures in adapter
```

**Labels:** 
- `area:unreal`
- `area:webrtc`
- `bug`
- `audio`

**Body Template:**
```markdown
Part of Epic: #91 (UPDATE with actual epic number)
Phase: A.1 - Unmask and Contain

[Copy full content from Issue 1 section in /tmp/webrtc_audio_refactor_issues.md]
```

**Repeat for each issue:**
- Issue 2: Phase A.2 - Decouple audio capture component
- Issue 3: Phase B.3 - Centralize audio setup
- Issue 4: Phase B.4 - Enforce strict ordering
- Issue 5: Phase C.5 - Polish logging and diagnostics
- Issue 6: Phase C.6 - Create test matrix
- Issue 7: Phase D.7 - Cleanup and documentation

### Step 3: Link Sub-Issues to Epic

After creating all issues, edit the Epic issue body to add:

```markdown
## Sub-Issues

### Phase A: Unmask and Contain
- [ ] #XX - Unmask EnableAudioSend failures in adapter
- [ ] #YY - Decouple audio capture component from WebRTC negotiation

### Phase B: Centralize and Order  
- [ ] #ZZ - Wire audio before transport start in BroadcastComponent
- [ ] #AA - Enforce strict ordering in WebRTCConnector

### Phase C: Diagnostics and Tests
- [ ] #BB - Polish logging and status diagnostics
- [ ] #CC - Create comprehensive test matrix

### Phase D: Cleanup
- [ ] #DD - Final cleanup and documentation
```

## Issue Content Location

All detailed issue content is available in:
- **Detailed specs:** `/tmp/webrtc_audio_refactor_issues.md` (8 issues with full details)
- **Summary:** `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md` (executive overview)

## Recommended Labels

Ensure these labels exist in the repository:
- `epic` - For the tracking issue
- `area:unreal` - Unreal Engine specific work
- `area:webrtc` - WebRTC functionality
- `audio` - Audio-related features
- `bug` - For Issue #1 (fixing masked failures)
- `refactor` - For Issues #2, #3, #4 (architectural changes)
- `enhancement` - For Issue #5 (diagnostics improvement)
- `testing` - For Issue #6 (test creation)
- `documentation` - For Issue #7 (docs and cleanup)

## Issue Dependencies

Some issues depend on others being complete first:

- **Issue 3** depends on **Issue 2** (needs new AudioCaptureComponent API)
- **Issue 5** depends on **Issues 1-4** (diagnostics for refactored system)
- **Issue 6** depends on **Issues 1-5** (testing complete refactor)
- **Issue 7** depends on **all previous issues** (final cleanup)

Mark these dependencies in issue descriptions or project board.

## Verification Checklist

After creating all issues:
- [ ] Epic issue created with appropriate labels
- [ ] All 7 sub-issues created
- [ ] Each sub-issue references the Epic in its body
- [ ] Epic issue body links all sub-issues
- [ ] Labels applied correctly
- [ ] Dependencies noted in issue descriptions
- [ ] Issue numbers recorded for team reference

## Alternative: Batch Creation Script

If you prefer automation, here's a Python script outline:

```python
#!/usr/bin/env python3
import subprocess
import re

REPO = "lifelike-and-believable/Open3DStream"
ISSUES_FILE = "/tmp/webrtc_audio_refactor_issues.md"

# Read and parse issues
with open(ISSUES_FILE, 'r') as f:
    content = f.read()

# Extract each issue and create via gh CLI
for issue_num in range(1, 8):
    # Extract title, labels, body for issue
    # ... parsing logic ...
    
    # Create issue
    subprocess.run([
        "gh", "issue", "create",
        "--repo", REPO,
        "--title", title,
        "--label", labels,
        "--body", body
    ])
```

Save as `create_issues.py` and run: `python3 create_issues.py`

---

## Support

If you encounter issues:
1. Check that labels exist in the repository
2. Verify you have permission to create issues
3. Ensure GitHub CLI is authenticated: `gh auth status`
4. Review issue content in `/tmp/webrtc_audio_refactor_issues.md` for completeness

For questions about the refactor itself, refer to:
- `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md` (original refactor plan)
- `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md` (executive summary)
