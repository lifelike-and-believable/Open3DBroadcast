markdown
# Audio-Synchronized Mocap Playback Implementation Plan

**Project**: Open3DStream - WebRTC LiveKit Receiver Animation Stability Fix  
**Repository**: lifelike-and-believable/Open3DStream  
**Branch**: develop (commit: 0c781f68)  
**Date**: 2025-11-07  
**Author**: atgoldberg

---

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

---

## Technical Context

### Existing Implementation

#### Current Frame Flow (WebRTC Path)

LiveKit → FOpen3DSWebRtcReceiver::OnConnectorData() 
       → [COALESCES - keeps only latest]
       → FOpen3DStreamSource::OnPackage()
       → [Immediate LiveLink application]


#### Current Audio Flow

RTP Audio → FO3DSOpusDecoder::DecodeRtpPacket()
         → PublishAudio()
         → FO3DSAudioBus::PublishPcm16()
         → USoundWaveProcedural (playout)


#### Key Files & Classes

| Component | File Path | Purpose |
|-----------|-----------|---------|
| LiveLink Source | `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp` | Receives and applies mocap frames |
| WebRTC Receiver | `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTC/Open3DSWebRtcReceiver.cpp` | WebRTC-specific receiver adapter |
| Audio Bus | `plugins/unreal/Open3DStream/Source/Open3DStream/Public/O3DSAudioBus.h` | Global audio frame distribution |
| Opus Decoder | `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTC/O3DSOpusDecoder.cpp` | RTP → PCM16 decoding |
| Settings | `plugins/unreal/Open3DStream/Source/Open3DStream/Public/Open3DStreamSourceSettings.h` | User-configurable parameters |

### Protocol Information

#### SubjectList Metadata
- `mTime` (double): Protocol timestamp in seconds
- `tx_seq` (uint64): Monotonic sequence number (feature branch `feature/tx-seq-wallclock`)
- `tx_wallclock_us` (uint64): Wall-clock timestamp in microseconds

#### Audio Timing
- **RTP Timestamp**: Sample-based counter at 48 kHz
- **AudioClock**: Unreal's submix callback provides wall-clock playout position
- **TimestampSec**: Converted timestamp in `FAudioFrameMeta` structure

---

## Implementation Plan

### Phase 1: Audio Clock Infrastructure

#### Task 1.1: Add Global Audio Playback Time Tracking
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Public/O3DSAudioBus.h`

**Changes**:
cpp
// Add after existing includes
#include "HAL/ThreadSafeBool.h"
#include "Misc/ScopeLock.h"

class OPEN3DSTREAM_API FO3DSAudioBus
{
public:
    // Existing: static FO3DSOnAudioPcm16& OnPcm16()
    
    // NEW: Audio clock tracking
    static void UpdateAudioPlayoutTime(double TimeSeconds)
    {
        static FCriticalSection Mutex;
        static double GLastAudioTime = 0.0;
        
        FScopeLock Lock(&Mutex);
        GLastAudioTime = TimeSeconds;
    }
    
    static double GetAudioPlayoutTime()
    {
        static FCriticalSection Mutex;
        static double GLastAudioTime = 0.0;
        
        FScopeLock Lock(&Mutex);
        return GLastAudioTime;
    }

    static void PublishPcm16(const O3DS::FAudioFrameMeta& Meta, const uint8* Data, int32 NumBytes)
    {
        // NEW: Update global audio clock from metadata
        if (Meta.TimestampSec > 0.0)
        {
            UpdateAudioPlayoutTime(Meta.TimestampSec);
        }
        
        // Existing broadcast code (unchanged)
        TArray<uint8> Copy;
        if (NumBytes > 0 && Data)
        {
            Copy.Append(Data, NumBytes);
        }
        OnPcm16().Broadcast(Meta, Copy);
    }
};


**Testing**:
- Console command to query current audio time
- Verify monotonic increase during playback

---

### Phase 2: Jitter Buffer Data Structures

#### Task 2.1: Add Timestamped Frame Buffer to LiveLink Source
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Public/Open3DStreamSource.h`

**Changes**:
cpp
class OPEN3DSTREAM_API FOpen3DStreamSource : public ILiveLinkSource, /.../
{
public:
    // Existing members unchanged...
    
private:
    // NEW: Timestamped frame buffer for audio sync
    struct FTimestampedFrame
    {
        uint64_t Sequence;       // Monotonic frame counter
        double Timestamp;        // From SubjectList.mTime
        TArray<uint8> Data;      // Serialized frame data
        double ArrivalTime;      // FPlatformTime::Seconds() when received
    };
    
    TArray<FTimestampedFrame> FrameBuffer;
    uint64_t LastAppliedSeq = 0;
    
    // Configuration (expose via settings later)
    float BufferDelaySeconds = 0.1f;  // 100ms default
    int32 MaxBufferFrames = 10;       // Prevent unbounded growth
    
    // NEW: Buffer management methods
    void BufferMocapFrame(uint64_t Seq, double Timestamp, const TArray<uint8>& Data);
    void ProcessBufferedFrames();
};


**Dependencies**: None  
**Estimated Effort**: 30 minutes

---

#### Task 2.2: Implement Frame Buffering Logic
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

**Add new methods** (insert before `OnPackage`):

cpp
void FOpen3DStreamSource::BufferMocapFrame(uint64_t Seq, double Timestamp, const TArray<uint8>& Data)
{
    // Drop old/duplicate sequences
    if (Seq <= LastAppliedSeq)
    {
        if (CVarO3DSReceiverDebugParse->GetInt() != 0)
        {
            UE_LOG(LogO3DSReceiver, Verbose, TEXT("Dropped seq=%llu (last=%llu)"), Seq, LastAppliedSeq);
        }
        return;
    }
    
    // Add to buffer
    FTimestampedFrame Frame;
    Frame.Sequence = Seq;
    Frame.Timestamp = Timestamp;
    Frame.Data = Data;
    Frame.ArrivalTime = FPlatformTime::Seconds();
    
    FrameBuffer.Add(Frame);
    
    // Sort by sequence (handles out-of-order arrivals)
    FrameBuffer.Sort([](const FTimestampedFrame& A, const FTimestampedFrame& B) {
        return A.Sequence < B.Sequence;
    });
    
    // Prevent unbounded growth - drop oldest if too many
    while (FrameBuffer.Num() > MaxBufferFrames)
    {
        UE_LOG(LogO3DSReceiver, Warning, TEXT("Buffer overflow - dropping frame seq=%llu"), 
               FrameBuffer[0].Sequence);
        FrameBuffer.RemoveAt(0);
    }
}

void FOpen3DStreamSource::ProcessBufferedFrames()
{
    if (FrameBuffer.Num() == 0)
        return;
    
    // Get current audio playout time
    const double AudioTime = FO3DSAudioBus::GetAudioPlayoutTime();
    const double TargetPlayoutTime = AudioTime + BufferDelaySeconds;
    
    // Process all frames that are "due" based on audio clock
    while (FrameBuffer.Num() > 0)
    {
        const FTimestampedFrame& Frame = FrameBuffer[0];
        
        // Check if this frame should play now
        if (Frame.Timestamp <= TargetPlayoutTime)
        {
            // Apply the frame (call existing OnPackage logic)
            OnPackageInternal(Frame.Data); // Renamed to avoid recursion
            LastAppliedSeq = Frame.Sequence;
            
            FrameBuffer.RemoveAt(0);
            
            if (CVarO3DSReceiverDebugParse->GetInt() != 0)
            {
                UE_LOG(LogO3DSReceiver, Verbose, 
                       TEXT("Applied frame seq=%llu ts=%.3f (audio=%.3f)"), 
                       Frame.Sequence, Frame.Timestamp, AudioTime);
            }
        }
        else
        {
            // Frame is not due yet - wait for audio to catch up
            break;
        }
    }
}


**Testing**:
- Verify frames are buffered in sequence order
- Confirm buffer cap prevents memory growth
- Check logging output

**Estimated Effort**: 1 hour

---

### Phase 3: Integrate Buffering into Frame Pipeline

#### Task 3.1: Refactor OnPackage to Use Buffering
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

**Modify existing `OnPackage` method** (around line 236):

cpp
void FOpen3DStreamSource::OnPackage(const TArray<uint8>& data)
{
    // UNCHANGED: Game thread marshaling
    if (!IsInGameThread())
    {
        TWeakPtr<FOpen3DStreamSource> WeakSelf = AsShared();
        TArray<uint8> Copy = data;
        AsyncTask(ENamedThreads::GameThread, [WeakSelf, Copy = MoveTemp(Copy)]() mutable
        {
            if (TSharedPtr<FOpen3DStreamSource> Pinned = WeakSelf.Pin())
            {
                Pinned->OnPackage(Copy);
            }
        });
        return;
    }
    
    if (!bIsValid)
        return;

    // Parse frame to extract metadata
    const double ParseStartWall = FPlatformTime::Seconds();
    if (!mSubjects.Parse((const char*)data.GetData(), data.Num(), 0))
    {
        OnStatus(LOCTEXT("DataError", "Data Error"), true);
        return;
    }

    const double SubjectListTime = mSubjects.mTime; // Protocol timestamp
    const uint64_t CurrentSeq = Frame++; // Use existing frame counter
    
    // NEW: Buffer frame instead of immediate apply
    BufferMocapFrame(CurrentSeq, SubjectListTime, data);
    
    // Note: Actual application happens in Tick() -> ProcessBufferedFrames()
}


**Create new internal method** `OnPackageInternal` by copying the existing frame application logic:

cpp
void FOpen3DStreamSource::OnPackageInternal(const TArray<uint8>& data)
{
    // This is the EXISTING OnPackage logic from line 250 onwards
    // (all the mSubjects parsing, LiveLink frame creation, etc.)
    // Just renamed to avoid calling BufferMocapFrame recursively
    
    // Copy entire block from current OnPackage starting at:
    // "const double SubjectListTime = mSubjects.mTime;"
    // through the end of the function
}


**Estimated Effort**: 45 minutes

---

#### Task 3.2: Update Tick to Process Buffer
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

**Modify `Tick` method** (around line 203):

cpp
void FOpen3DStreamSource::Tick(float DeltaTime)
{
    TimeSinceLastCheck += DeltaTime;
    if (TimeSinceLastCheck >= CheckInterval)
    {
        RemoveInactiveSubjects();
        TimeSinceLastCheck = 0;
    }

    // Tick WebRTC receiver if active
    if (WebRtcReceiver)
    {
        WebRtcReceiver->Tick(DeltaTime);
    }
    else
    {
        this->server.tick();
    }
    
    // NEW: Process buffered frames based on audio clock
    ProcessBufferedFrames();
}


**Testing**:
- Verify frames play out smoothly
- Check timing alignment with console debug output
- Test with/without audio

**Estimated Effort**: 15 minutes

---

### Phase 4: User Configuration & Settings

#### Task 4.1: Add Audio Sync Settings to Source Settings
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Public/Open3DStreamSourceSettings.h`

**Add to `FOpen3DStreamSettings` struct**:

cpp
USTRUCT(BlueprintType)
struct FOpen3DStreamSettings
{
    GENERATED_BODY()

    // Existing settings...
    
    // NEW: Audio-sync buffer settings
    UPROPERTY(EditAnywhere, Category="Open3DStream|Sync", 
              meta=(ClampMin="0.0", ClampMax="2.0", 
                    ToolTip="Buffer delay in seconds for audio/mocap sync (0-2s). Higher = more stable but more latency."))
    float AudioSyncBufferSeconds = 0.1f; // 100ms default
    
    UPROPERTY(EditAnywhere, Category="Open3DStream|Sync",
              meta=(ToolTip="Enable audio-driven playback (syncs mocap to audio clock). Disable for immediate mode."))
    bool bEnableAudioSync = true;
    
    UPROPERTY(EditAnywhere, Category="Open3DStream|Sync",
              meta=(ClampMin="3", ClampMax="30",
                    ToolTip="Maximum buffered frames before oldest are dropped (prevents memory growth)"))
    int32 MaxBufferedFrames = 10;
};


**Estimated Effort**: 20 minutes

---

#### Task 4.2: Wire Settings into Source Constructor
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

**Modify constructor**:

cpp
FOpen3DStreamSource::FOpen3DStreamSource(const FOpen3DStreamSettings& Settings)
    : /* existing initialization */
{
    // Existing code...
    
    // NEW: Configure buffer from settings
    BufferDelaySeconds = Settings.AudioSyncBufferSeconds;
    MaxBufferFrames = Settings.MaxBufferedFrames;
    bAudioSyncEnabled = Settings.bEnableAudioSync; // Add this member variable
    
    // Store settings for later use
    SourceSettings = Settings;
}


**Also update header**:
cpp
// In Open3DStreamSource.h, add member:
bool bAudioSyncEnabled = true;


**Estimated Effort**: 15 minutes

---

### Phase 5: Console Commands & Debugging Tools

#### Task 5.1: Add Console Variables
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

**Add at top of file** (with other CVars around line 20):

cpp
static TAutoConsoleVariable<int32> CVarO3DSReceiverAudioSync(
    TEXT("o3ds.Receiver.AudioSync"),
    1,
    TEXT("Enable audio-synchronized mocap playback (0=off, 1=on)"),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarO3DSReceiverBufferDelay(
    TEXT("o3ds.Receiver.BufferDelay"),
    0.1f,
    TEXT("Audio sync buffer delay in seconds (0.0-2.0)"),
    ECVF_Default);


**Read CVars in ProcessBufferedFrames**:

cpp
void FOpen3DStreamSource::ProcessBufferedFrames()
{
    // Allow runtime override via CVar
    const float RuntimeBufferDelay = CVarO3DSReceiverBufferDelay->GetFloat();
    const bool bSyncEnabled = (CVarO3DSReceiverAudioSync->GetInt() != 0) && bAudioSyncEnabled;
    
    if (!bSyncEnabled)
    {
        // Immediate mode: apply all buffered frames instantly
        while (FrameBuffer.Num() > 0)
        {
            const FTimestampedFrame& Frame = FrameBuffer[0];
            OnPackageInternal(Frame.Data);
            LastAppliedSeq = Frame.Sequence;
            FrameBuffer.RemoveAt(0);
        }
        return;
    }
    
    // Rest of audio-sync logic using RuntimeBufferDelay...
}


**Estimated Effort**: 30 minutes

---

#### Task 5.2: Add Buffer Dump Console Command
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`

**Add static registry and command**:

cpp
// Global registry for debug access
static TArray<TWeakPtr<FOpen3DStreamSource>> GActiveSources;

// In FOpen3DStreamSource constructor, register:
FOpen3DStreamSource::FOpen3DStreamSource(...)
{
    // Existing code...
    GActiveSources.Add(AsWeak());
}

// In destructor, unregister:
FOpen3DStreamSource::~FOpen3DStreamSource()
{
    GActiveSources.RemoveAll([this](const TWeakPtr<FOpen3DStreamSource>& Weak) {
        return !Weak.IsValid() || Weak.Pin().Get() == this;
    });
}

// Console command
static FAutoConsoleCommand CmdDumpMocapBuffer(
    TEXT("o3ds.Receiver.DumpBuffer"),
    TEXT("Dump current mocap frame buffer state for all active sources"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        int32 SourceIndex = 0;
        for (const TWeakPtr<FOpen3DStreamSource>& Weak : GActiveSources)
        {
            if (TSharedPtr<FOpen3DStreamSource> Source = Weak.Pin())
            {
                UE_LOG(LogO3DSReceiver, Log, TEXT("Source %d: BufferSize=%d LastSeq=%llu AudioTime=%.3f"),
                    SourceIndex,
                    Source->FrameBuffer.Num(),
                    Source->LastAppliedSeq,
                    FO3DSAudioBus::GetAudioPlayoutTime());
                
                for (int32 i = 0; i < Source->FrameBuffer.Num(); ++i)
                {
                    const auto& Frame = Source->FrameBuffer[i];
                    UE_LOG(LogO3DSReceiver, Log, TEXT("  [%d] Seq=%llu Ts=%.3f Age=%.3fs"),
                        i, Frame.Sequence, Frame.Timestamp,
                        FPlatformTime::Seconds() - Frame.ArrivalTime);
                }
                SourceIndex++;
            }
        }
        
        if (SourceIndex == 0)
        {
            UE_LOG(LogO3DSReceiver, Log, TEXT("No active sources"));
        }
    }));


**Estimated Effort**: 45 minutes

---

### Phase 6: Remove Aggressive Coalescing from WebRTC Receiver

#### Task 6.1: Replace Coalescing with Queue in WebRTC Receiver
**File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/WebRTC/Open3DSWebRtcReceiver.cpp`

**Current problematic code** (around line 217):

cpp
void FOpen3DSWebRtcReceiver::OnConnectorData(const TArray<uint8>& Bytes)
{
    // CURRENT: Coalesce to keep only latest (CAUSES JERKINESS)
    {
        FScopeLock L(&CoalesceMutex);
        PendingData = Bytes; // OVERWRITES previous frame
        // ...
    }
}


**NEW: Pass through without coalescing**:

cpp
void FOpen3DSWebRtcReceiver::OnConnectorData(const TArray<uint8>& Bytes)
{
    // Remove coalescing - let jitter buffer handle it
    // Just marshal to game thread
    TWeakPtr<FOpen3DSWebRtcReceiver> WeakSelf = AsShared();
    TArray<uint8> Copy = Bytes;
    
    AsyncTask(ENamedThreads::GameThread, [WeakSelf, Copy = MoveTemp(Copy)]()
    {
        if (TSharedPtr<FOpen3DSWebRtcReceiver> Self = WeakSelf.Pin())
        {
            if (Self->OnDataCallback)
            {
                Self->OnDataCallback(Copy);
            }
        }
    });
}


**Remove unused members** from `Open3DSWebRtcReceiver.h`:

cpp
// DELETE these lines:
FCriticalSection CoalesceMutex;
TArray<uint8> PendingData;
TAtomic<bool> bDataDispatchScheduled{false};


**Testing**:
- Verify no frames are dropped unnecessarily
- Check jitter buffer receives all frames
- Monitor buffer size with dump command

**Estimated Effort**: 30 minutes

---

### Phase 7: Documentation & Testing

#### Task 7.1: Create Implementation Document
**New File**: `docs/AUDIO_SYNC_IMPLEMENTATION.md`

**Content**:

markdown
# Audio-Synchronized Mocap Playback

## Overview
Implements a jitter buffer that synchronizes mocap playback with audio playout position, eliminating jerky animation on WebRTC receivers.

## Architecture

### Components
1. *FO3DSAudioBus*: Global audio clock tracking
2. *FOpen3DStreamSource*: Jitter buffer and frame scheduling
3. *FTimestampedFrame*: Buffered frame with sequence and timestamp

### Frame Flow

WebRTC → Receiver → Buffer (sorted by seq) → Process (audio-timed) → LiveLink


## Configuration

### Settings (Open3DStreamSourceSettings)
- AudioSyncBufferSeconds (0-2s): Playout delay for smoothness
- bEnableAudioSync (bool): Enable/disable sync
- MaxBufferedFrames (3-30): Cap to prevent memory growth

### Console Commands

o3ds.Receiver.AudioSync 0/1          - Toggle sync on/off
o3ds.Receiver.BufferDelay 0.2        - Adjust delay at runtime
o3ds.Receiver.DumpBuffer             - Show buffer state
o3ds.Receiver.DebugParse 1           - Verbose frame logging


## Troubleshooting

### Jerky Animation
- Increase AudioSyncBufferSeconds to 0.2-0.5s
- Check audio is playing (buffer needs audio clock)
- Verify frames arriving: o3ds.Receiver.DumpBuffer

### Audio/Mocap Drift
- Audio timestamp must be reliable
- Check TimestampSec in FAudioFrameMeta
- RTP timestamp conversion: ts / 48000.0

### Buffer Overflow
- Increase MaxBufferedFrames
- Or decrease AudioSyncBufferSeconds
- Network may be too unstable (packet loss)


**Estimated Effort**: 1 hour

---

#### Task 7.2: Update Testing Guide
**File**: `plugins/unreal/Open3DStream/docs/WEBRTC_TESTING_GUIDE.md`

**Add section**:

markdown
## Testing Audio-Synchronized Playback

### Setup
1. Broadcaster with audio and mocap enabled
2. Receiver with bEnableWebRTCAudio = true
3. UO3DSRemoteAudioComponent on character

### Test Cases

#### TC1: Smooth Startup
*Steps*:
1. Start broadcaster
2. Connect receiver
3. Observe first 5 seconds

*Expected*: Animation starts smoothly, no jerks or speed-ups  
*Verify*: o3ds.Receiver.DumpBuffer shows 2-5 frames buffered

#### TC2: Audio Lip Sync
*Steps*:
1. Play dialogue audio with visemes
2. Observe mouth movements

*Expected*: Mouth matches audio with <100ms lag  
*Verify*: Use video recording + frame analysis

#### TC3: Reconnect Stability
*Steps*:
1. Stop broadcaster
2. Wait 5 seconds
3. Restart broadcaster

*Expected*: Animation resumes smoothly within 2 seconds  
*Verify*: No crash, buffer rebuilds

#### TC4: Buffer Tuning
*Steps*:
1. Set AudioSyncBufferSeconds = 0.05 (50ms)
2. Observe animation
3. Set AudioSyncBufferSeconds = 0.5 (500ms)
4. Observe again

*Expected*: 
- 50ms: More responsive but might jitter
- 500ms: Very smooth but noticeable delay

### Metrics

o3ds.Receiver.DumpBuffer

Source 0: BufferSize=3 LastSeq=1245 AudioTime=12.456
  [0] Seq=1246 Ts=12.500 Age=0.044s
  [1] Seq=1247 Ts=12.517 Age=0.027s
  [2] Seq=1248 Ts=12.533 Age=0.011s


- *BufferSize*: Should be 2-8 frames normally
- *Age*: Frames <0.2s old are healthy
- *Seq gaps*: None expected (check for drops)


**Estimated Effort**: 45 minutes

---

#### Task 7.3: Add Unit Tests
**New File**: `plugins/unreal/Open3DStream/Source/Open3DStream/Private/Tests/AudioSyncBufferTests.cpp`

**Content**:

cpp
#include "Misc/AutomationTest.h"
#include "Open3DStreamSource.h"
#include "O3DSAudioBus.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioClockTest, 
    "Open3DStream.AudioSync.AudioClock",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAudioClockTest::RunTest(const FString& Parameters)
{
    // Test audio clock updates
    FO3DSAudioBus::UpdateAudioPlayoutTime(1.0);
    TestEqual("Audio time set", FO3DSAudioBus::GetAudioPlayoutTime(), 1.0, 0.001);
    
    FO3DSAudioBus::UpdateAudioPlayoutTime(2.5);
    TestEqual("Audio time updated", FO3DSAudioBus::GetAudioPlayoutTime(), 2.5, 0.001);
    
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBufferOrderingTest,
    "Open3DStream.AudioSync.BufferOrdering",
    EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBufferOrderingTest::RunTest(const FString& Parameters)
{
    // Test frame ordering (out-of-order arrival)
    // Mock FOpen3DStreamSource and verify buffer sorts correctly
    
    // TODO: Implementation requires test fixture
    
    return true;
}


**Estimated Effort**: 2 hours

---

## Task Summary & Sequence

### Phase 1: Audio Clock (30 min)
- [ ] Task 1.1: Add audio time tracking to FO3DSAudioBus

### Phase 2: Data Structures (1.5 hrs)
- [ ] Task 2.1: Add FTimestampedFrame struct to header
- [ ] Task 2.2: Implement BufferMocapFrame and ProcessBufferedFrames

### Phase 3: Integration (1 hr)
- [ ] Task 3.1: Refactor OnPackage to buffer frames
- [ ] Task 3.2: Update Tick to process buffer

### Phase 4: Configuration (35 min)
- [ ] Task 4.1: Add settings to FOpen3DStreamSettings
- [ ] Task 4.2: Wire settings into constructor

### Phase 5: Debugging (1.25 hrs)
- [ ] Task 5.1: Add console variables
- [ ] Task 5.2: Add buffer dump command

### Phase 6: Remove Coalescing (30 min)
- [ ] Task 6.1: Remove aggressive coalescing from WebRTC receiver

### Phase 7: Documentation (3.75 hrs)
- [ ] Task 7.1: Create implementation doc
- [ ] Task 7.2: Update testing guide
- [ ] Task 7.3: Add unit tests

**Total Estimated Effort**: ~8.5 hours

---

## Testing Checklist

### Functional Tests
- [ ] Animation smooth on initial connect (no jerks)
- [ ] Audio/mocap lip sync within 100ms
- [ ] Buffer grows/shrinks appropriately
- [ ] No memory leaks (buffer cap works)
- [ ] Console commands work correctly
- [ ] Settings persist and apply
- [ ] Immediate mode works (AudioSync=0)

### Regression Tests
- [ ] TCP/UDP/NNG transports still work
- [ ] Non-audio mocap still works
- [ ] Multiple LiveLink sources work
- [ ] PIE start/stop cycles stable
- [ ] Broadcaster restart recovers

### Performance Tests
- [ ] No frame drops in buffer
- [ ] CPU usage <5% for buffering logic
- [ ] Memory stable over 10min session
- [ ] 60fps animation maintained

---

## Dependencies & Prerequisites

### Code Dependencies
- Existing `FO3DSAudioBus` infrastructure
- `FOpen3DStreamSource` LiveLink implementation
- `FOpen3DSWebRtcReceiver` adapter
- `SubjectList.mTime` protocol timestamp

### Feature Branch Integration
The `feature/tx-seq-wallclock` branch adds:
- `tx_seq`: Monotonic sequence number
- `tx_wallclock_us`: Wall-clock timestamp

**Recommendation**: Merge this branch first for production-quality sequencing.

### Build Requirements
- Unreal Engine 5.x (version in use)
- Open3DStream plugin compiled
- LiveKit FFI library available

---

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

---

## Risk Mitigation

### Risk: Audio clock not available
**Mitigation**: Fallback to immediate mode if `GetAudioPlayoutTime() == 0.0`

### Risk: Excessive buffer growth
**Mitigation**: Hard cap at `MaxBufferedFrames` with logging

### Risk: Timestamp discontinuities
**Mitigation**: Detect large jumps (>1s) and reset buffer

### Risk: Performance impact
**Mitigation**: Profile ProcessBufferedFrames; optimize if >2ms/frame

---

## References

### Related Issues
- Jerky animation on WebRTC connect (current issue)
- Audio late-binding fix (#141-#143)
- LiveKit backend implementation (#145-#147)

### Documentation
- `LIVEKIT_README.md`: Tuning guide
- `WEBRTC_TESTING_GUIDE.md`: Testing procedures
- `feature/tx-seq-wallclock` branch: Sequence number implementation

### Code Locations

plugins/unreal/Open3DStream/
├── Source/Open3DStream/
│   ├── Public/
│   │   ├── O3DSAudioBus.h                    (Phase 1)
│   │   ├── Open3DStreamSource.h              (Phase 2)
│   │   └── Open3DStreamSourceSettings.h      (Phase 4)
│   └── Private/
│       ├── Open3DStreamSource.cpp            (Phases 2,3,5)
│       └── WebRTC/
│           ├── Open3DSWebRtcReceiver.cpp     (Phase 6)
│           └── O3DSOpusDecoder.cpp           (Reference)
└── docs/
    ├── AUDIO_SYNC_IMPLEMENTATION.md          (Phase 7)
    └── WEBRTC_TESTING_GUIDE.md               (Phase 7)


---

## Implementation Notes

### Thread Safety
- `FO3DSAudioBus::UpdateAudioPlayoutTime()` uses `FCriticalSection`
- `BufferMocapFrame()` called only on game thread (after marshal)
- `ProcessBufferedFrames()` called from `Tick()` (game thread)

### Performance Considerations
- Frame buffer sort is O(n log n) but n typically <10
- Consider TCircularBuffer if buffer churn becomes issue
- Audio clock read is lock-protected but fast (<1µs)

### Backward Compatibility
- New settings default to enabled (opt-out)
- CVars allow runtime disable for testing
- Existing transports unaffected (no code changes)

---

**End of Implementation Plan**

*This document provides a complete, actionable plan for implementing audio-synchronized mocap playback in Open3DStream's WebRTC receiver path.*


This comprehensive markdown document contains:
- ✅ Complete problem analysis
- ✅ Technical context with file paths
- ✅ 7 implementation phases with 13 tasks
- ✅ Detailed code snippets for each change
- ✅ Testing procedures and success criteria
- ✅ Time estimates (~8.5 hours total)
- ✅ Risk mitigation strategies
- ✅ References to existing codebase

A planning agent can use this to generate a sequence of GitHub issues, pull requests, or task tickets.
