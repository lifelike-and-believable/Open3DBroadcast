# WebRTC Audio Refactor - Planning Complete ✅

**Date:** 2025-10-30  
**Planning Agent:** GitHub Copilot  
**Source:** `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`

## 📋 Summary

The WebRTC Audio Refactor plan has been analyzed and decomposed into **8 discrete, executable GitHub Issues** ready for delegation to coding agents.

## 📦 What Was Created

### 1. Executive Summary
**File:** `WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md`

A high-level overview containing:
- Problem statement for each issue
- Priority and dependencies
- Effort estimates
- Implementation strategy
- Key design decisions
- Success criteria

**Use this for:** Team review, stakeholder communication, project planning

### 2. Issue Creation Guide  
**File:** `WEBRTC_AUDIO_REFACTOR_ISSUES_HOWTO.md`

Step-by-step instructions for:
- Creating issues via GitHub CLI
- Creating issues via web interface
- Linking issues to Epic
- Managing dependencies
- Label setup

**Use this for:** Actually creating the GitHub issues

### 3. Detailed Issue Specifications
**File:** `/tmp/webrtc_audio_refactor_issues.md` (temporary location)

Complete specifications for all 8 issues including:
- Problem statements with root cause analysis
- Required code changes with line numbers
- Code examples showing exact changes needed
- Testable acceptance criteria
- References to source files
- Dependencies on other issues

**Use this for:** Issue body content when creating GitHub issues

## 🎯 Issue Overview

| Issue | Title | Phase | Priority | Estimate | Dependencies |
|-------|-------|-------|----------|----------|--------------|
| Epic | WebRTC Audio Path Refactor | - | - | 3 days | - |
| #1 | Unmask EnableAudioSend failures | A.1 | High | 0.5 day | - |
| #2 | Decouple audio capture component | A.2 | High | 1 day | - |
| #3 | Centralize audio setup | B.3 | High | 1 day | #2 |
| #4 | Enforce strict ordering | B.4 | High | 1 day | - |
| #5 | Polish logging and diagnostics | C.5 | Medium | 0.5 day | #1-4 |
| #6 | Create test matrix | C.6 | Medium | 0.5 day | #1-5 |
| #7 | Cleanup and documentation | D.7 | Low | 0.5 day | #1-6 |

**Total: 5 days of work compressed to 3 days with buffering**

## 🔑 Key Insights

### Critical Ordering Pattern

From the working libdatachannel example (`examples/audio-comm-test/client.cpp` lines 142-168):

```cpp
// 1. Create PeerConnection
pc = createPeerConnection(config, ws);

// 2. Add audio track FIRST (ensures inclusion in initial SDP offer)
audioTrack = pc->addTrack(media);

// 3. Create DataChannel SECOND (triggers offer/answer exchange)
dc = pc->createDataChannel("test");
```

**Why this matters:** Adding the audio track BEFORE the DataChannel ensures the track is included in the initial SDP offer. If added after, audio won't be negotiated without manual renegotiation.

### Root Problems Being Fixed

1. **Silent Failures:** Adapter masks errors from inner connector
2. **Fragile Timing:** AudioCaptureComponent participates in negotiation
3. **Unclear Ordering:** Setup sequence not deterministic
4. **Poor Diagnostics:** Hard to debug audio issues

### Solution Architecture

```
UO3DSBroadcastComponent
  ↓ (owns setup sequence)
  ├─→ Computes StreamLabel
  ├─→ Calls EnableAudioSend() BEFORE Start
  ├─→ Wires AudioCaptureComponent
  └─→ Starts transport

UO3DSBroadcastAudioCaptureComponent
  ↓ (pure PCM source)
  └─→ SetStreamLabel() + SetAudioSink() + PushFrames()

FWebRTCConnector
  ↓ (sole negotiation owner)
  ├─→ EnableAudioSend() (before PC exists)
  ├─→ Start() creates PC
  ├─→ SetupPeerConnection():
  │     1. Add audio track
  │     2. Create DataChannel  
  │     3. Create offer
  └─→ PushPcm() encodes and sends
```

## 🚀 Next Steps

### Immediate Actions (Today)

1. **Review the summary document**
   ```bash
   cat WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md
   ```

2. **Review the detailed specifications**
   ```bash
   cat /tmp/webrtc_audio_refactor_issues.md | less
   ```

3. **Discuss with team:** Priorities, timeline, resource allocation

### Issue Creation (Tomorrow)

4. **Create Epic issue first** (to get issue number)
   - Follow instructions in `WEBRTC_AUDIO_REFACTOR_ISSUES_HOWTO.md`
   - Note the Epic issue number (e.g., #91)

5. **Create sub-issues 1-7**
   - Reference Epic in each issue body
   - Apply correct labels
   - Note dependencies

6. **Link issues together**
   - Update Epic with sub-issue numbers
   - Mark dependencies in project board

### Implementation (This Week)

7. **Start Phase A** (Issues #1-2)
   - Assign to coding agent or developer
   - These are foundational and unblock other work

8. **Continue with Phases B, C, D**
   - Follow dependency chain
   - Each phase builds on previous

## 📁 File Locations

```
Open3DStream/
├── WEBRTC_AUDIO_REFACTOR_PLANNING_COMPLETE.md  ← You are here
├── WEBRTC_AUDIO_REFACTOR_ISSUES_SUMMARY.md     ← Executive summary
├── WEBRTC_AUDIO_REFACTOR_ISSUES_HOWTO.md       ← Creation guide
└── plugins/unreal/Open3DStream/
    └── docs/
        └── WEBRTC_AUDIO_REFACTOR.md            ← Original plan

/tmp/
└── webrtc_audio_refactor_issues.md             ← Detailed specs
```

## ✅ Quality Assurance

Each issue specification includes:
- ✅ Clear problem statement with root cause
- ✅ Specific file paths and line numbers
- ✅ Code examples showing required changes
- ✅ Testable acceptance criteria
- ✅ References to original plan and working examples
- ✅ Dependencies clearly stated

## 🎓 Background Reading

For deeper understanding:
1. **Original plan:** `plugins/unreal/Open3DStream/docs/WEBRTC_AUDIO_REFACTOR.md`
2. **Working example:** https://github.com/lifelike-and-believable/libdatachannel/blob/master/examples/audio-comm-test/client.cpp
3. **Current implementation:**
   - `Source/Open3DStream/Private/WebRTCConnectorFactory.cpp`
   - `Source/Open3DStream/Private/O3DSBroadcastAudioCaptureComponent.cpp`
   - `Source/Open3DStream/Private/WebRTCConnector.cpp`
   - `Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`

## 💡 Tips for Coding Agents

When implementing these issues:
1. **Read the detailed spec carefully** - includes exact line numbers and code
2. **Reference the working example** - shows the pattern that works
3. **Follow the acceptance criteria** - they are testable and specific
4. **Respect dependencies** - Issue #3 needs #2 complete first, etc.
5. **Test incrementally** - each issue should compile and partially work
6. **Update the Epic** - check off completed issues in the Epic body

## 🆘 Support

If you have questions about:
- **The plan:** Review original doc and summary
- **Creating issues:** See HOWTO guide
- **Implementation:** Read detailed specs in `/tmp/webrtc_audio_refactor_issues.md`
- **Architecture:** See "Solution Architecture" section above

## ✨ Success Metrics

After all issues are complete, you should see:
- ✅ Audio track in SDP 100% of the time when enabled
- ✅ Zero silent failures (errors are visible and logged)
- ✅ Clean separation of concerns (no negotiation in capture component)
- ✅ Deterministic setup (same sequence every time)
- ✅ Comprehensive diagnostics (`o3ds.WebRTC.Audio.Status`)
- ✅ Full test coverage (unit + integration)
- ✅ Complete documentation

---

**Status:** 🟢 Planning Complete - Ready for Issue Creation

**Next Owner:** Team Lead / Project Manager (for issue creation)

**Then:** Coding Agents (for implementation)
