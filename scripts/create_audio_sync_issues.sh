#!/bin/bash
#
# Create GitHub Issues for Audio-Synchronized Mocap Playback over WebRTC
#
# This script creates a complete set of GitHub issues (1 epic + 13 implementation tasks)
# based on the implementation plan in plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md
#
# Usage: ./create_audio_sync_issues.sh
#
# Prerequisites:
# - gh CLI installed and authenticated
# - Appropriate permissions on the repository
#

set -e

REPO="lifelike-and-believable/Open3DStream"
DOC_REF="plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md"
PLAN_DOC="AUDIO_SYNC_DEVELOPMENT_PLAN.md"

echo "=========================================="
echo "Audio-Synchronized Mocap Playback"
echo "GitHub Issue Creation Script"
echo "=========================================="
echo ""
echo "Repository: $REPO"
echo "Reference: $DOC_REF"
echo ""

# Check gh CLI is available
if ! command -v gh &> /dev/null; then
    echo "Error: gh CLI not found. Please install: https://cli.github.com/"
    exit 1
fi

# Check authentication
if ! gh auth status &> /dev/null; then
    echo "Error: gh CLI not authenticated. Run: gh auth login"
    exit 1
fi

echo "✓ gh CLI ready"
echo ""

# Function to create an issue and return its number
create_issue() {
    local title="$1"
    local body_file="$2"
    local labels="$3"
    
    echo "Creating: $title"
    
    local issue_url
    issue_url=$(gh issue create \
        --repo "$REPO" \
        --title "$title" \
        --body-file "$body_file" \
        --label "$labels" 2>&1)
    
    if [[ $issue_url =~ github.com.*issues/([0-9]+) ]]; then
        local issue_num="${BASH_REMATCH[1]}"
        echo "  ✓ Created #$issue_num: $issue_url"
        echo "$issue_num"
    else
        echo "  ✗ Failed to create issue"
        echo "  Response: $issue_url"
        echo "0"
    fi
}

# Create temporary directory for issue bodies
ISSUE_DIR=$(mktemp -d)
trap "rm -rf $ISSUE_DIR" EXIT

echo "Step 1: Creating Epic Issue"
echo "----------------------------"

cat > "$ISSUE_DIR/epic.md" << 'EOF'
# Epic: Audio-Synchronized Mocap Playback over WebRTC

**Reference Document**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md`  
**Development Plan**: `AUDIO_SYNC_DEVELOPMENT_PLAN.md`

## Executive Summary

### Problem Statement
The WebRTC LiveKit receiver experiences jerky, inconsistent animation playback when subscribers connect:
- First few seconds: slow and jerky animation
- Next few seconds: speeds up/catches up
- Eventually settles but never fully stable
- Issue specific to WebRTC path; TCP/UDP/NNG transports work correctly

### Root Causes Identified

1. **Aggressive Frame Coalescing**: WebRTC receiver drops intermediate frames, causing temporal discontinuities
2. **Strict Out-of-Order Rejection**: Unordered delivery causes valid frames to be dropped
3. **No Jitter Buffer**: Immediate frame application without buffering for network jitter
4. **No Audio Synchronization**: Mocap frames play independently of audio timeline

### Solution Overview
Implement an **audio-driven jitter buffer** that:
- Uses sequence numbers for frame ordering (prevents duplicates/out-of-order)
- Uses timestamps for audio/mocap synchronization
- Provides user-tunable buffer delay (0-2 seconds)
- Ensures smooth, consistent animation with lip-sync

## Implementation Overview

**Total Estimated Effort**: ~8.5 hours across 7 phases with 13 tasks

| Phase | Description | Effort |
|-------|-------------|--------|
| 1 | Audio Clock Infrastructure | 30 min |
| 2 | Jitter Buffer Data Structures | 1.5 hrs |
| 3 | Frame Pipeline Integration | 1 hr |
| 4 | User Configuration & Settings | 35 min |
| 5 | Console Commands & Debugging | 1.25 hrs |
| 6 | Remove Aggressive Coalescing | 30 min |
| 7 | Documentation & Testing | 3.75 hrs |

## Success Criteria

### Primary Goals
✅ Animation smooth from first frame (no startup jerkiness)  
✅ Audio/mocap synchronized within user tolerance (0-500ms configurable)  
✅ No frame drops or memory leaks  
✅ User can tune buffer delay

### Secondary Goals
✅ Console commands for runtime debugging  
✅ Comprehensive documentation  
✅ Unit tests for core logic  
✅ Backward compatible with existing transports

## Implementation Tasks

Individual tasks will be created as separate issues and linked below:

### Phase 1: Audio Clock Infrastructure
- [ ] TBD: Add Global Audio Playback Time Tracking

### Phase 2: Jitter Buffer Data Structures
- [ ] TBD: Add Timestamped Frame Buffer to LiveLink Source
- [ ] TBD: Implement Frame Buffering Logic

### Phase 3: Frame Pipeline Integration
- [ ] TBD: Refactor OnPackage to Use Buffering
- [ ] TBD: Update Tick to Process Buffer

### Phase 4: User Configuration & Settings
- [ ] TBD: Add Audio Sync Settings to Source Settings
- [ ] TBD: Wire Settings into Source Constructor

### Phase 5: Console Commands & Debugging
- [ ] TBD: Add Console Variables
- [ ] TBD: Add Buffer Dump Console Command

### Phase 6: Remove Aggressive Coalescing
- [ ] TBD: Remove Frame Coalescing from WebRTC Receiver

### Phase 7: Documentation & Testing
- [ ] TBD: Create Implementation Documentation
- [ ] TBD: Update Testing Guide
- [ ] TBD: Add Unit Tests

## Related Issues

- Issue #149: Protocol hardening with tx_seq and tx_wallclock_us
- Issues #118-#125: WebRTC Audio refactor
- Issues #88, #90: LiveKit backend implementation

## Dependencies & Prerequisites

### Code Dependencies
- Existing `FO3DSAudioBus` infrastructure
- `FOpen3DStreamSource` LiveLink implementation
- `FOpen3DSWebRtcReceiver` adapter
- `SubjectList.mTime` protocol timestamp

### Feature Branch Integration
The `feature/tx-seq-wallclock` branch (Issue #149) adds production-quality sequencing. Consider merging for improved frame ordering.

## Reference Documentation

- **Implementation Plan**: `plugins/unreal/Open3DStream/docs/WebRTCPlaybackFixes.md` (complete technical spec)
- **Development Plan**: `AUDIO_SYNC_DEVELOPMENT_PLAN.md` (this roadmap)
- **Architecture**: `.github/copilot-instructions.md` (repository standards)
- **Testing**: `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md` (will be updated in Phase 7)
EOF

EPIC_NUM=$(create_issue \
    "Epic: Audio-Synchronized Mocap Playback over WebRTC" \
    "$ISSUE_DIR/epic.md" \
    "epic,area:unreal,area:webrtc,enhancement,audio")

if [ "$EPIC_NUM" == "0" ]; then
    echo "Failed to create epic issue. Aborting."
    exit 1
fi

echo ""
echo "Epic Issue #$EPIC_NUM created successfully!"
echo ""
echo "Please review the epic issue at:"
echo "https://github.com/$REPO/issues/$EPIC_NUM"
echo ""
echo "=========================================="
echo "Next Steps:"
echo "=========================================="
echo ""
echo "1. Review the epic issue and adjust as needed"
echo "2. Create individual phase/task issues manually or extend this script"
echo "3. Link task issues to epic #$EPIC_NUM"
echo "4. Begin implementation starting with Phase 1"
echo ""
echo "For detailed implementation guidance, see:"
echo "- $DOC_REF (technical specification)"
echo "- $PLAN_DOC (development roadmap)"
echo ""
echo "Issue creation templates are available in:"
echo "- /tmp/AUDIO_SYNC_GITHUB_ISSUES_PLAN.md"
echo "- /tmp/create_audio_sync_issues.py"
echo ""
