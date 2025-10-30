# WebRTC Audio Refactor - Issue Planning 🎯

> **Status:** ✅ Planning Complete - Ready for GitHub Issue Creation  
> **Date:** 2025-10-30  
> **Estimated Effort:** 3 days (5 days unbuffered)

## 📖 Quick Start

### For Project Managers
1. Read: `WEBRTC_AUDIO_REFACTOR_PLANNING_COMPLETE.md` (5 min)
2. Review: `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md` (10 min)
3. Create: Follow `WEBRTC_AUDIO_REFACTOR_ISSUES_HOWTO.md` (30 min)

### For Developers
1. Understand the problem: Read original `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`
2. See the solution: Review `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md` 
3. Study the pattern: Check libdatachannel example at https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp
4. Implement: Pick up issues from GitHub once created

## 📚 Document Guide

| Document | Purpose | Audience | Time |
|----------|---------|----------|------|
| **README_WEBRTC_AUDIO_REFACTOR.md** | This file - quick navigation | Everyone | 2 min |
| **WEBRTC_AUDIO_REFACTOR_PLANNING_COMPLETE.md** | Main summary and next steps | PM, Team Lead | 5 min |
| **WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md** | Executive overview of all issues | PM, Developers | 10 min |
| **WEBRTC_AUDIO_REFACTOR_ISSUES_HOWTO.md** | How to create GitHub issues | PM, Team Lead | 30 min |
| **WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md** | Detailed specs for all 8 issues | Developers | Full reference |

## 🎯 The 8 Issues

```
Epic: WebRTC Audio Path Refactor
├── Phase A: Unmask and Contain (1.5 days)
│   ├── Issue #1: Unmask EnableAudioSend failures [Bug] 🔴
│   └── Issue #2: Decouple audio capture component [Refactor] 🔴
│
├── Phase B: Centralize and Order (2 days)
│   ├── Issue #3: Centralize audio setup [Refactor] 🔴 (depends on #2)
│   └── Issue #4: Enforce strict ordering [Refactor] 🔴
│
├── Phase C: Diagnostics and Tests (1 day)
│   ├── Issue #5: Polish logging and diagnostics [Enhancement] 🟡 (depends on #1-4)
│   └── Issue #6: Create test matrix [Testing] 🟡 (depends on #1-5)
│
└── Phase D: Cleanup (0.5 day)
    └── Issue #7: Cleanup and documentation [Docs] 🟢 (depends on #1-6)

Legend: 🔴 High Priority | 🟡 Medium Priority | 🟢 Low Priority
```

## 🔑 The Core Problem

**Current:** Audio track sometimes missing from SDP, setup fails silently

**Root Causes:**
1. Adapter masks connector failures
2. AudioCaptureComponent participates in timing-sensitive negotiation
3. Setup order not enforced
4. Poor diagnostics

**Solution:** Enforce this pattern (from working libdatachannel example):

```cpp
// Step 1: Create PeerConnection
pc = createPeerConnection();

// Step 2: Add audio track FIRST ⭐
audioTrack = pc->addTrack(media);

// Step 3: Create DataChannel SECOND
dc = pc->createDataChannel("test");

// Step 4: Offer/answer happens (audio included in SDP)
```

## ⚡ Quick Commands

```bash
# View the main summary
cat WEBRTC_AUDIO_REFACTOR_PLANNING_COMPLETE.md

# View executive summary
cat WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md

# View issue creation guide
cat WEBRTC_AUDIO_REFACTOR_ISSUES_HOWTO.md

# View detailed issue specs
cat WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md | less

# Create Epic issue (example - see HOWTO for full automation)
gh issue create --repo lifelike-and-believable/Open3DStream \
  --title "[EPIC] WebRTC Audio Path Refactor" \
  --label "epic,area:unreal,area:webrtc,audio" \
  --body-file <(sed -n '/## Issue 8/,/^---$/p' WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md)
```

## 🎓 Background

**Original Plan:** `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`  
**Working Example:** https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp

**Components Affected:**
- `WebRTCConnectorFactory.cpp` (adapter)
- `O3DSBroadcastAudioCaptureComponent.cpp` (capture)
- `O3DSBroadcastComponent.cpp` (setup orchestration)
- `WebRTCConnector.cpp` (negotiation)

## ✅ Success Criteria

After implementation:
- ✅ Audio track in SDP 100% when enabled
- ✅ Zero silent failures
- ✅ Clean component separation
- ✅ Deterministic setup sequence
- ✅ Comprehensive diagnostics
- ✅ Full test coverage

## 🚀 Implementation Timeline

| Week | Phase | Issues | Deliverable |
|------|-------|--------|-------------|
| Week 1, Day 1-2 | Phase A | #1, #2 | Unmask failures, decouple components |
| Week 1, Day 3-4 | Phase B | #3, #4 | Centralize setup, enforce ordering |
| Week 1, Day 5 | Phase C | #5, #6 | Diagnostics and tests |
| Week 2, Day 1 | Phase D | #7 | Cleanup and docs |

## 📞 Support

**Questions about the plan?** → Read `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md`  
**Questions about creating issues?** → Read `WEBRTC_AUDIO_REFACTOR_ISSUES_HOWTO.md`  
**Questions during implementation?** → Check detailed specs in `WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md`

---

**Ready to proceed:** Create the Epic issue and sub-issues, then start Phase A! 🎉
