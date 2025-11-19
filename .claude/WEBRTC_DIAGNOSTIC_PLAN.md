# WebRTC Transport Diagnostic & Remediation Plan

**Created:** 2025-11-19
**Status:** In Progress
**Critical Issue:** WebRTC transport not working for single or multiple subjects despite successful connection to LiveKit media server

---

## Executive Summary

After comprehensive investigation, we've determined that:

1. **Sender and receiver successfully connect to LiveKit media server** ✓
2. **Data is NOT reaching the O3DReceiverSource for processing** ✗
3. **This affects single-subject AND multi-subject scenarios equally** ✗

The root cause is unknown - data could be failing at any of these points:
- OnDataReceivedEx callback never fires (LiveKit FFI issue)
- Frames are queued but Poll() never retrieves them (race condition)
- Frames reach consumer but FlatBuffer parse fails (serialization issue)
- Payload is corrupted in transit (LiveKit FFI issue)

All other transports (Loopback, TCP/UDP Sockets, NNG) are working correctly.

---

## PRIORITY 1: Implement Per-Subject Audio Label Integration with New LiveKit FFI Callback

### Objective
Update WebRTC receiver to use the new `lk_client_set_audio_callback_ex()` callback with per-subject audio labels instead of the current fallback mechanism using `LastObservedSubjectName`.

### Why This First
The new callback provides **proper per-subject audio labels** which is:
1. **Foundational** for correct multi-subject audio routing
2. **Required** before diagnosing/validating the mocap data flow
3. **Available now** (no waiting for external dependencies)
4. **The correct architecture** that the system should be built on

Once audio labels work correctly, we can then validate/fix the mocap data channels with full confidence.

### Implementation Plan

#### Task 1: Implement New Labeled Audio Callback in WebRTCReceiver

**Location:** [WebRTCReceiver.cpp:200-236](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp#L200)

**Current Implementation (Fallback - Remove):**
```cpp
lk_client_set_audio_callback(ClientHandle, FO3DWebRTCReceiver::OnAudioReceived, this);
```

The current `OnAudioReceived()` callback:
- Does NOT receive per-subject labels
- Sets audio label to `"audio_default"` for all audio
- Routes audio via `LastObservedSubjectName` fallback

**New Implementation (Replace With):**
```cpp
// Register new labeled audio callback
lk_client_set_audio_callback_ex(ClientHandle, FO3DWebRTCReceiver::OnAudioReceivedEx, this);
```

**New Callback Function to Create:**
```cpp
static void FO3DWebRTCReceiver::OnAudioReceivedEx(
    void* user,
    const char* subject_label,      // NEW: Per-subject audio label from LiveKit FFI
    const int16_t* pcm_interleaved,
    size_t frames_per_channel,
    int32_t channels,
    int32_t sample_rate)
{
    FO3DWebRTCReceiver* Self = reinterpret_cast<FO3DWebRTCReceiver*>(user);
    if (!Self) return;

    // Extract subject label from parameter (no longer need fallback)
    FString SubjectLabel = subject_label && *subject_label
        ? FString(subject_label)
        : TEXT("audio_default");

    // Create audio metadata with EXPLICIT subject label
    O3DS::FAudioFrameMeta Meta;
    Meta.SampleRate = sample_rate;
    Meta.NumChannels = channels;
    Meta.StreamLabel = SubjectLabel;  // Direct label from callback, not fallback
    Meta.SourceGuid = Self->SourceGuid;

    // Submit to audio sink with correct subject association
    if (Self->AudioSink)
    {
        Self->AudioSink->SubmitPcm16(Meta, reinterpret_cast<const uint8*>(pcm_interleaved),
            frames_per_channel * channels * sizeof(int16));
    }
}
```

**Success Criteria:**
- ✅ New callback registered successfully
- ✅ `subject_label` parameter received from LiveKit FFI
- ✅ Audio label explicitly set from callback (no fallback needed)
- ✅ Audio routed to correct subject by label

#### Task 2: Remove LastObservedSubjectName Fallback Logic

**Location:** [O3DReceiverSource.cpp:745-801](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DReceiver/Private/O3DReceiverSource.cpp#L745)

**Current Code to Remove/Update:**
```cpp
void FO3DReceiverSource::FinalizeAudioMeta(O3DS::FAudioFrameMeta& Meta) const
{
    // ...

    // REMOVE THIS SECTION - no longer needed with proper labels
    if (bMetaLooksLikeFallback && !LastObservedSubjectName.IsNone())
    {
        Meta.SubjectName = LastObservedSubjectName.ToString();  // FALLBACK - DELETE
    }
}
```

**New Implementation:**
```cpp
void FO3DReceiverSource::FinalizeAudioMeta(O3DS::FAudioFrameMeta& Meta) const
{
    // Meta.StreamLabel already contains subject label from callback
    // No fallback logic needed - audio is explicitly routed per-subject
    // Just ensure subject name is properly set
    Meta.SubjectName = Meta.StreamLabel;  // Direct mapping from label
}
```

**Success Criteria:**
- ✅ Fallback mechanism completely removed
- ✅ Audio routed exclusively by explicit subject labels
- ✅ No LastObservedSubjectName dependency

#### Task 3: Test Per-Subject Audio with New Callback

**Test Setup:**
1. Create sender with 2 subjects, each with audio
2. Create receiver configured for both subjects
3. Create 2 RemoteAudioComponents listening to different subjects

**Test Scenario:**
```
Subject "Actor_A": Plays tone frequency 440Hz (A note)
Subject "Actor_B": Plays tone frequency 880Hz (A note one octave higher)

RemoteAudioComponent_A listens to "Actor_A" → hears 440Hz
RemoteAudioComponent_B listens to "Actor_B" → hears 880Hz
```

**Validation:**
- ✅ Each component hears ONLY its subject's audio
- ✅ No audio cross-talk or mixing
- ✅ Labels explicitly passed through callback
- ✅ Can distinguish subject A's audio from subject B's audio

**Logging to Add:**
```cpp
UE_LOG(LogO3DWebRTCReceiver, Warning,
    TEXT("OnAudioReceivedEx: subject_label='%s' frames=%zu channels=%d sample_rate=%d"),
    subject_label ? subject_label : "(NULL)", frames_per_channel, channels, sample_rate);
```

**Success Criteria:**
- ✅ Audio from different subjects is properly isolated
- ✅ Each subject's audio routed to correct RemoteAudioComponent
- ✅ No fallback logic invoked

---

## PRIORITY 2: Verify WebRTC Mocap Data Architecture (Phase 0 - AFTER Audio Fixed)

### Objective
Once audio callback is properly implemented with per-subject labels, verify that the WebRTC mocap data serialization and routing architecture is correct.

### Implementation Plan

#### Task 0.1: Audit WebRTCSender Per-Subject Serialization Logic

**Verification Points:**
1. For each subject in input SubjectList, create separate SingleSubjectList
2. Copy ONLY that subject's data (mJoints, mCurveNames, mCurveValues, mContext, mTransforms)
3. Serialize each SingleSubjectList independently
4. Send each serialized payload with subject name as label via `lk_send_data_ex()`

**Code Location:** [WebRTCSender.cpp:411-557](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp#L411)

**Checklist:**
- [ ] Line 448: SingleSubjectList created via `addSubject()` call
- [ ] Lines 456-469: Data copied (mJoints, mCurveNames, mCurveValues, mContext, mTransforms)
- [ ] Line 472-473: Serialization happens on SingleSubjectList (not full List)
- [ ] Line 512-516: Subject label extracted from `Subject->mName`
- [ ] Line 520-527: `lk_send_data_ex()` called with subject name as label parameter
- [ ] Line 483: Transform list cleared to prevent double-deletion

**Success Criteria:**
- Code matches intended architecture
- Each subject gets own serialized payload
- Each subject gets unique label
- No data corruption during subject isolation

#### Task 0.2: Audit WebRTCReceiver Per-Subject Buffering Logic

**Verification Points:**
1. OnDataReceivedEx receives label parameter (subject name)
2. Frames buffered per-subject in `PendingFramesBySubject[label]`
3. Poll() retrieves latest frame per subject
4. Consumer receives one call per subject per frame interval

**Code Location:** [WebRTCReceiver.cpp:163-193, 389-471](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp#L163)

**Checklist:**
- [ ] Line 169: Label parameter properly converted to FString
- [ ] Lines 176-185: Frames queued with label as key in PendingFramesBySubject
- [ ] Lines 398-410: Poll iterates PendingFramesBySubject by subject
- [ ] Lines 406-407: Latest frame kept, intermediate dropped per subject
- [ ] Line 447: Consumer->SubmitFrame called once per subject per poll

**Success Criteria:**
- Code matches intended architecture
- Per-subject buffering working
- No frames lost due to logic errors
- Label routing intact

#### Task 0.3: Verify Per-Subject Audio Track Creation (Sender Side)

**Verification Points:**
1. Audio sink maintains `TMap<FString, LkAudioTrackHandle*>` for per-subject tracks
2. GetOrCreateAudioTrack() creates unique track per subject
3. Audio published to subject-specific track via track handle

**Code Location:** [WebRTCSender.cpp:49-52, 147-191](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp#L49)

**Checklist:**
- [ ] Line 49-52: AudioTracks map defined with per-subject storage
- [ ] Line 147-191: GetOrCreateAudioTrack creates new track per StreamLabel
- [ ] Audio tracks created with unique names per subject
- [ ] Cleanup destroys all tracks on disconnect

**Success Criteria:**
- Audio tracks created per subject
- No track reuse between subjects
- Proper cleanup on disconnect

#### Task 0.3b: Verify Per-Subject Audio Routing (Receiver Side)

**Verification Points:**
1. Receiver's audio callback correctly associates audio with subject
2. Audio metadata includes subject name or label information
3. Audio routed to correct RemoteAudioComponent based on subject

**Code Location:** [WebRTCReceiver.cpp:200-236](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/Private/Receiver/WebRTCReceiver.cpp#L200), [O3DReceiverSource.cpp:745-801](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DReceiver/Private/O3DReceiverSource.cpp#L745)

**Current Implementation Check:**
- [ ] Line 219 (WebRTCReceiver): Audio callback sets `Meta.StreamLabel = TEXT("audio_default")`
- [ ] Line 764-773 (O3DReceiverSource): FinalizeAudioMeta uses `LastObservedSubjectName` fallback
- [ ] This is a WORKAROUND until LiveKit FFI provides per-subject audio labels

**Limitation Documentation:**
- [ ] Document that current audio routing uses last-observed-subject-name fallback
- [ ] Note that this works for single subject but may fail with concurrent audio sources
- [ ] Mark as "TODO: Switch to lk_client_set_audio_callback_ex() when available"

**Success Criteria:**
- Understand current audio routing mechanism
- Identify where per-subject labels will be integrated when LiveKit FFI update available
- Document the limitation clearly for future work

#### Task 0.4: Check for Recent Refactor Regressions

**Search For:**
- Any recent changes to Send() method that might have broken per-subject logic
- Any changes to subject list iteration (potential off-by-one errors)
- Any changes to label generation that might produce invalid labels
- Any recent API usage changes that don't match header files

**Compare Against:**
- [o3ds/model.h](ProjectSandbox/Plugins/Open3DBroadcast/ThirdParty/open3dstream/include/o3ds/model.h) for actual SubjectList API
- [livekit_ffi.h](ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/include/livekit_ffi.h) for FFI signatures
- Git history: commit `ce9cf6e` (API usage fixes)

**Red Flags to Watch For:**
- Using non-existent methods (e.g., `.at()` instead of `mItems[]`)
- Wrong member access (e.g., `mRef` instead of `mReference`)
- Incomplete data copying (missing some subject fields)
- Label encoding issues (UTF-8 round-trip problems)

**Success Criteria:**
- No regressions found, OR
- Regressions identified and documented for fix

#### Task 0.5: Log Architectural Verification

Add temporary logging to confirm per-subject architecture is being executed:

**In WebRTCSender::Send():**
```cpp
UE_LOG(LogO3DWebRTCSender, Warning,
    TEXT("Send() START: Input list has %zu subjects"),
    List.mItems.size());

for (size_t SubjectIdx = 0; SubjectIdx < List.mItems.size(); ++SubjectIdx)
{
    O3DS::Subject* Subject = List.mItems[SubjectIdx];

    UE_LOG(LogO3DWebRTCSender, Warning,
        TEXT("  Processing subject[%zu]: name='%s' (creating SingleSubjectList)"),
        SubjectIdx, Subject->mName.c_str());

    // ... create SingleSubjectList ...

    UE_LOG(LogO3DWebRTCSender, Warning,
        TEXT("  SingleSubjectList created: %zu transforms, label='%s'"),
        NewSubject->mTransforms.mItems.size(),
        Subject->mName.c_str());

    // ... serialize and send ...

    UE_LOG(LogO3DWebRTCSender, Warning,
        TEXT("  Sent subject[%zu] with label='%s' (%d bytes)"),
        SubjectIdx, Subject->mName.c_str(), BytesWritten);
}

UE_LOG(LogO3DWebRTCSender, Warning,
    TEXT("Send() END: Processed %zu subjects, %d succeeded"),
    List.mItems.size(), SubjectsProcessed);
```

**In WebRTCReceiver::OnDataReceivedEx():**
```cpp
UE_LOG(LogO3DWebRTCReceiver, Warning,
    TEXT("OnDataReceivedEx ENTRY: label='%s' bytes=%zu (received on per-subject channel)"),
    label ? label : "(NULL)", len);
```

**In WebRTCReceiver::Poll():**
```cpp
UE_LOG(LogO3DWebRTCReceiver, Warning,
    TEXT("Poll() START: %d subjects in PendingFramesBySubject"),
    PendingFramesBySubject.Num());

for (auto& FrameEntry : LatestFrameBySubject)
{
    UE_LOG(LogO3DWebRTCReceiver, Warning,
        TEXT("  Processing subject='%s': %d bytes (per-subject frame)"),
        *FrameEntry.Key, FrameEntry.Value.Payload.Num());
}
```

**Success Criteria:** Logs confirm per-subject architecture being executed

---

### Success Criteria for Phase 0

✓ Sender code correctly implements per-subject serialization
✓ Receiver code correctly buffers frames per-subject
✓ Audio tracks created per-subject
✓ No regressions from recent refactors
✓ Logs confirm architecture in use

**If Phase 0 passes:** Proceed to Phase 1 (diagnose why it's not working)
**If Phase 0 fails:** Fix architectural issues before diagnosing

---

## Phase 1: Single-Subject Failure Diagnosis

### Objective
Isolate the exact point where single-subject data transmission fails between sender and receiver.

### Implementation Plan

#### Task 1.1: Add Diagnostic Logging at Critical Checkpoints

**Location:** WebRTCSender.cpp, line ~520 (Send method)
```cpp
// Before sending data, log what we're about to send
UE_LOG(LogO3DWebRTCSender, Warning,
    TEXT("SEND [%s]: subject='%s' bytes=%d reliable=%s label_utf8='%s'"),
    ANSI_TO_TCHAR(__FUNCTION__),
    Subject->mName.c_str(),
    BytesWritten,
    (Reliability == LkReliable ? TEXT("true") : TEXT("false")),
    TCHAR_TO_UTF8(*SubjectLabel));
```

**Location:** WebRTCReceiver.cpp, line ~163 (OnDataReceivedEx callback - entry point)
```cpp
// CRITICAL: Confirm callback is invoked at all
UE_LOG(LogO3DWebRTCReceiver, Warning,
    TEXT("CALLBACK [OnDataReceivedEx]: label='%s' bytes=%zu reliability=%d (CALLBACK FIRED)"),
    label ? label : "(NULL)",
    len,
    reliability);
```

**Location:** WebRTCReceiver.cpp, line ~389 (Poll method - start)
```cpp
// Check if any frames in queue
int32 TotalFrames = 0;
int32 TotalSubjects = 0;
{
    FScopeLock Lock(&PendingFramesMutex);
    TotalSubjects = PendingFramesBySubject.Num();
    for (const auto& Q : PendingFramesBySubject)
        TotalFrames += Q.Value.Num();
}

if (TotalFrames > 0)
{
    UE_LOG(LogO3DWebRTCReceiver, Warning,
        TEXT("POLL [%s]: %d subjects, %d total frames in queue"),
        ANSI_TO_TCHAR(__FUNCTION__),
        TotalSubjects, TotalFrames);
}
```

**Location:** WebRTCReceiver.cpp, line ~447 (Poll method - frame submission)
```cpp
// Log every frame submission
UE_LOG(LogO3DWebRTCReceiver, Warning,
    TEXT("POLL [SubmitFrame]: subject='%s' bytes=%d"),
    *SubjectLabel,
    Frame.Payload.Num());

Consumer->SubmitFrame(SubjectLabel, Frame.Payload, Frame.EnqueueTimeSeconds);
```

**Location:** O3DReceiverSource.cpp, line ~464 (ParseSubjectListBuffer)
```cpp
// Log parse attempt and result
bool bParseSuccess = SubjectScratch.Parse(
    reinterpret_cast<const char*>(Buffer.GetData()),
    Buffer.Num(),
    nullptr,
    true);

if (bParseSuccess)
{
    UE_LOG(LogO3DReceiverSource, Warning,
        TEXT("PARSE [%s]: SUCCESS subject='%s' bytes=%d subjects_in_list=%d timestamp=%.6f"),
        ANSI_TO_TCHAR(__FUNCTION__),
        *Subject,
        Buffer.Num(),
        SubjectScratch.mItems.size(),
        SubjectScratch.mTime);
}
else
{
    UE_LOG(LogO3DReceiverSource, Error,
        TEXT("PARSE [%s]: FAILED subject='%s' bytes=%d error='%s'"),
        ANSI_TO_TCHAR(__FUNCTION__),
        *Subject,
        Buffer.Num(),
        *SubjectScratch.mError);
}
```

**Success Criteria:** When test runs, logs show data progression (or stoppage) at each checkpoint.

#### Task 1.2: Run Single-Subject Test

**Setup:**
1. Launch sender with 1 subject only
2. Launch receiver
3. Verify both connect to LiveKit (check connection logs)
4. Verify sender sends frames (check "SEND" logs)
5. Monitor receiver logs for progression

**Expected Log Progression:**
```
SEND [Send]: subject='MyActor' bytes=1234 reliable=true label_utf8='MyActor'
SEND [Send]: subject='MyActor' bytes=1234 reliable=true label_utf8='MyActor'
...
CALLBACK [OnDataReceivedEx]: label='MyActor' bytes=1234 reliability=1 (CALLBACK FIRED)
CALLBACK [OnDataReceivedEx]: label='MyActor' bytes=1234 reliability=1 (CALLBACK FIRED)
...
POLL [Poll]: 1 subjects, 1 total frames in queue
POLL [SubmitFrame]: subject='MyActor' bytes=1234
PARSE [ParseSubjectListBuffer]: SUCCESS subject='MyActor' bytes=1234 subjects_in_list=1 timestamp=123.456789
PARSE [ParseSubjectListBuffer]: SUCCESS subject='MyActor' bytes=1234 subjects_in_list=1 timestamp=123.456790
...
```

**Success Criteria:** All 5 checkpoints log successfully in sequence.

#### Task 1.3: Determine Failure Point

**If SEND logs appear but CALLBACK logs don't:**
→ Problem is LiveKit FFI not delivering data to receiver
→ Proceed to Task 1.4a (Debug LiveKit Connection)

**If CALLBACK logs appear but POLL logs show 0 frames:**
→ Problem is queue not being populated or race condition
→ Proceed to Task 1.4b (Debug Queue Population)

**If POLL logs show frames but PARSE logs show FAILED:**
→ Problem is FlatBuffer deserialization
→ Proceed to Task 1.4c (Debug FlatBuffer)

**If all logs progress but animation doesn't play:**
→ Problem is downstream of this transport layer
→ Proceed to Task 1.4d (Debug LiveLink Integration)

---

### Failure Resolution Tasks

#### Task 1.4a: Debug LiveKit Connection (if callback doesn't fire)

**Investigation:**
1. Verify `lk_client_set_data_callback_ex()` returned success in receiver Initialize
2. Check if data callback is registered BEFORE connection or AFTER
3. Verify receiver is actually subscribed to sender's published data
4. Check LiveKit server logs for data transmission confirmation

**Code Changes:**
- Add logging after `lk_client_set_data_callback_ex()` call to confirm registration
- Add logging in OnConnectionState to show connection state transitions
- Verify connection state is `LkConnConnected` before data arrives

**Success Criteria:** Callback fires when sender transmits data

#### Task 1.4b: Debug Queue Population (if callback fires but Poll sees 0 frames)

**Investigation:**
1. Check if PendingFramesMutex is causing deadlock
2. Verify callback is using correct label as queue key
3. Check if frames are being enqueued but immediately dequeued elsewhere
4. Verify no race between callback (LiveKit thread) and Poll (game thread)

**Code Changes:**
- Add logging at queue insertion point showing queue state before/after
- Add logging in Poll showing queue iteration details
- Verify no other code is clearing PendingFramesBySubject

**Success Criteria:** Poll sees frames in queue after callback fires

#### Task 1.4c: Debug FlatBuffer Parsing (if Parse fails)

**Investigation:**
1. Check if single-subject serialization is valid FlatBuffer format
2. Verify payload bytes match expected FlatBuffer signature
3. Check if SingleSubjectList creation is correct
4. Compare serialized bytes from sender with working Loopback transport

**Code Changes:**
- Add binary dump of first/last 16 bytes of payload
- Check FlatBuffer magic number (should be specific value)
- Add detailed FlatBuffer error logging from SubjectScratch.mError
- Compare with Loopback's serialized payload format

**Success Criteria:** Parse succeeds or shows detailed error reason

#### Task 1.4d: Debug LiveLink Integration (if parse succeeds but no animation)

**Investigation:**
1. Verify ProcessParsedSubject is called after successful parse
2. Check if PushSubjectStaticData succeeded (bone structure)
3. Check if PushSubjectFrameData succeeded (animation data)
4. Verify LiveLink subject is visible in engine

**Code Changes:**
- Add logging in ProcessParsedSubject
- Add logging before/after PushSubjectStaticData
- Add logging before/after PushSubjectFrameData
- Verify subject name in LiveLink matches sender subject name

**Success Criteria:** Animation appears in Unreal viewport

---

## Phase 2: Multi-Subject Validation

### Objective
Once single subject works, verify that multi-subject scenarios function correctly.

### Implementation Plan

#### Task 2.1: Extend Single-Subject Test to Multiple Subjects

**Setup:**
1. Modify sender to create 3 distinct subjects (e.g., "Actor", "Prop", "Camera")
2. Give each subject different transform data
3. Run test with 3 subjects

**Expected Behavior:**
- Each subject sends on its own labeled channel
- Each subject receives with its own label
- All 3 appear in LiveLink
- Each subject has correct transform data (not mixed with others)

**Logging Validation:**
- SEND logs show 3 different subject labels per frame
- CALLBACK logs show 3 different subject labels
- POLL logs show 3 subjects in queue
- PARSE logs show successful parse for each subject
- LiveLink has 3 subjects with correct data

**Success Criteria:** All 3 subjects animate correctly and independently

#### Task 2.2: Validate Per-Subject Data Channel Independence

**Test Scenario:**
1. Spawn 2 senders on same network to LiveKit
2. Sender A publishes subjects "Actor_A1", "Actor_A2"
3. Sender B publishes subjects "Actor_B1", "Actor_B2"
4. Receiver subscribes to all 4 subjects
5. Verify no cross-contamination

**Validation:**
- Each subject's label is unique
- Each subject's data matches its source (A_A1 data doesn't appear as B_B1)
- Can identify which sender each subject comes from by looking at labels
- No data loss when multiple senders active

**Success Criteria:** 4 distinct subjects visible, each with correct data

#### Task 2.3: Validate Per-Subject Audio Tracks

**Test Scenario:**
1. Enable audio on sender
2. Two subjects, each with different audio
3. Multiple RemoteAudioComponents configured for different subjects
4. Verify each hears correct audio for its subject

**Validation:**
- Audio tracks created per subject (check sender logs)
- Audio reaches receiver with subject association
- RemoteAudioComponent filters audio by subject name
- Can hear both audio streams simultaneously

**Success Criteria:** Each subject's audio plays independently and correctly

---

## Phase 3: Cross-Transport Validation

### Objective
Ensure WebRTC behaves identically to known-working transports.

### Implementation Plan

#### Task 3.1: Compare WebRTC vs. Loopback

**Setup:**
1. Create identical test with 2 subjects
2. Run with Loopback transport first (baseline)
3. Run with WebRTC transport
4. Compare results

**Validation Points:**
- Same subjects visible in LiveLink
- Same skeleton structure
- Same animation data
- Same transform values
- Same timing (frame rate, latency)

**Success Criteria:** Output is visually identical between transports

#### Task 3.2: Compare WebRTC vs. Sockets

**Setup:**
1. Run same test with TCP Sockets transport
2. Compare against WebRTC results

**Validation:**
- Same subject list
- Same animation fidelity
- Same latency characteristics

**Success Criteria:** WebRTC matches Sockets transport behavior

#### Task 3.3: Stress Test: 5+ Subjects

**Setup:**
1. Create 5+ subjects on sender
2. Run with WebRTC
3. Verify all animate correctly

**Validation:**
- No data loss
- No mixing between subjects
- Performance acceptable
- No memory leaks

**Success Criteria:** All subjects process without corruption

---

## Phase 4: Multi-Instance LiveKit Scenario

### Objective
Validate complete production scenario with multiple Unreal instances connected to shared LiveKit server.

### Implementation Plan

#### Task 4.1: Multi-Sender Scenario

**Setup:**
1. Instance A (Sender): Publishes "CharacterA" subject
2. Instance B (Sender): Publishes "CharacterB" subject
3. Instance C (Receiver): Subscribes to both
4. Verify Instance C sees both CharacterA and CharacterB

**Validation:**
- Both characters visible in Instance C
- Each character has correct animation
- No latency/sync issues
- No data corruption from concurrent senders

**Success Criteria:** Multi-sender coordination works correctly

#### Task 4.2: Multi-Receiver Scenario

**Setup:**
1. Instance A (Sender): Publishes "MainCharacter" subject
2. Instance B (Receiver): Subscribes
3. Instance C (Receiver): Subscribes
4. Verify both B and C see MainCharacter

**Validation:**
- Both receivers see identical data
- No latency difference
- Both can animate independently

**Success Criteria:** Multi-receiver fan-out works correctly

#### Task 4.3: Dynamic Connection Changes

**Setup:**
1. Start A (sender), B (receiver)
2. B sees data from A
3. Disconnect A
4. B stops receiving (gracefully)
5. Connect C (sender) with different character
6. B sees C's character

**Validation:**
- Graceful handling of sender disconnect
- Seamless transition to new sender
- No stale data corruption
- Connection state properly managed

**Success Criteria:** Dynamic reconnection scenarios work reliably

---

## Summary Table: Diagnostic Checkpoints

| Checkpoint | Location | Logs to Check | Success Sign |
|-----------|----------|---------------|--------------|
| 1. Data Sent | WebRTCSender::Send() | "SEND [Send]:" appears | Frames logged with subject name |
| 2. Callback Invoked | WebRTCReceiver::OnDataReceivedEx() | "CALLBACK [OnDataReceivedEx]:" appears | Callback logs match Send logs |
| 3. Queue Populated | WebRTCReceiver::Poll() start | "POLL [Poll]:" shows N>0 frames | Frame count > 0 |
| 4. Frame Submitted | WebRTCReceiver::Poll() submission | "POLL [SubmitFrame]:" appears | Subject names match Send logs |
| 5. Parse Succeeds | O3DReceiverSource::ParseSubjectListBuffer() | "PARSE [%s]: SUCCESS" or "FAILED" | SUCCESS appears in logs |

---

## Quick Reference: Failure Diagnosis

**Symptoms → Likely Cause → Next Task**

| Symptom | Cause | Task |
|---------|-------|------|
| SEND logs but no CALLBACK | LiveKit not delivering data | 1.4a |
| CALLBACK logs but POLL shows 0 frames | Queue not populated / race condition | 1.4b |
| POLL shows frames but PARSE FAILED | FlatBuffer serialization broken | 1.4c |
| PARSE SUCCESS but no animation | LiveLink integration issue | 1.4d |
| Single subject works, multi fails | Per-subject label/routing issue | 2.1 |

---

## Progress Tracking

### PRIORITY 1: Audio Label Integration (DO THIS FIRST)
- [ ] Task 1: Implement new OnAudioReceivedEx callback with per-subject labels
- [ ] Task 2: Remove LastObservedSubjectName fallback logic
- [ ] Task 3: Test per-subject audio isolation with 2 subjects
- [ ] Per-subject audio routing working ✓

### PRIORITY 2: Verify Mocap Data Architecture (AFTER Audio Fixed)

#### Phase 0: Verify Intended Architecture
- [ ] Task 0.1: Audit WebRTCSender per-subject serialization
- [ ] Task 0.2: Audit WebRTCReceiver per-subject buffering
- [ ] Task 0.3: Verify per-subject audio track creation (sender side)
- [ ] Task 0.4: Check for regressions from recent refactors
- [ ] Task 0.5: Add architectural verification logging
- [ ] Architecture verified ✓

#### Phase 1: Single-Subject Failure Diagnosis
- [ ] Task 1.1: Add diagnostic logging at 5 checkpoints
- [ ] Task 1.2: Run single-subject test
- [ ] Task 1.3: Identify failure point
- [ ] Task 1.4a/b/c/d: Fix root cause
- [ ] Single subject working ✓

#### Phase 2: Multi-Subject Validation
- [ ] Task 2.1: Extend to 3 subjects
- [ ] Task 2.2: Validate per-subject channel independence
- [ ] Task 2.3: Validate per-subject data + audio together
- [ ] Multiple subjects working ✓

#### Phase 3: Cross-Transport Validation
- [ ] Task 3.1: Compare vs. Loopback
- [ ] Task 3.2: Compare vs. Sockets
- [ ] Task 3.3: Stress test 5+ subjects
- [ ] Cross-transport match ✓

#### Phase 4: Multi-Instance LiveKit
- [ ] Task 4.1: Multi-sender scenario
- [ ] Task 4.2: Multi-receiver scenario
- [ ] Task 4.3: Dynamic connection changes
- [ ] Production scenario validated ✓

---

## Notes & Key Insights

### Architecture Understanding
- **Working transports** (Loopback, Sockets, NNG) serialize ALL subjects in one buffer, use one routing label per frame
- **WebRTC design** serializes EACH subject separately, uses per-subject labels for routing
- This difference is **intentional** to handle multiple concurrent senders without channel contention
- The consumer correctly handles both patterns

### Known Limitations & Upcoming Improvements
- ~~Audio callback from LiveKit FFI doesn't yet provide per-subject labels~~
- **IMPROVEMENT INCOMING:** LiveKit FFI team is adding new callback with per-subject audio labels
- Once available, can switch from `lk_client_set_audio_callback()` to new labeled callback
- Current fallback: audio routed by last-observed-subject-name (workaround until update available)
- **Action Required:** When new callback available, update WebRTCReceiver to:
  1. Register new labeled audio callback instead of generic callback
  2. Extract subject label from audio metadata
  3. Route audio directly to correct subject (removes fallback logic)
  4. Test per-subject audio with multiple concurrent subjects

### Risk Areas
1. **UTF-8 label encoding round-trip** - Subject names could get corrupted
2. **FlatBuffer format** - Small serialization errors break deserialization
3. **LiveKit FFI behavior** - Unclear if custom labels preserved or consolidated
4. **Thread safety** - LiveKit callback on background thread, Poll on game thread

---

## Related Documentation
- See `.claude/CLAUDE.md` for project rules and build settings
- See critical header files:
  - `ThirdParty/open3dstream/include/o3ds/model.h` (Subject/SubjectList APIs)
  - `ThirdParty/livekit_ffi/include/livekit_ffi.h` (LiveKit FFI APIs)
  - `Open3DShared/Public/SerializedFrameConsumerRegistry.h` (Consumer interface)

---

**Last Updated:** 2025-11-19
**Next Action:** Execute Phase 1, Task 1.1 (Add diagnostic logging)
