# Transport Design Comparison: Why WebRTC Handles SubjectList Differently

## Overview

The Open3DStream transport layer has four implementations: **Loopback**, **Sockets (TCP/UDP)**, **NNG**, and **WebRTC**. Each takes a fundamentally different approach to handling the incoming `SubjectList` in their `Send()` method. This document explains why WebRTC is unique and required special handling.

---

## 1. LOOPBACK TRANSPORT

**File**: `Open3DTransportLoopback/Private/Sender/LoopbackSender.cpp:142-195`

### Design Pattern
- **Serializes entire SubjectList as-is** (all subjects together)
- **Uses the received SubjectList directly** without modification
- Creates a single serialized buffer and enqueues it

### Send() Flow
```
Input: SubjectList (may contain multiple subjects)
  ↓
Serialize(List) → single buffer (all subjects)
  ↓
Enqueue packet with buffer
  ↓
Return (List still valid, received by-reference)
```

### Ownership Management
- **No ownership transfer**: The incoming `List` is read-only
- **No temporary objects created**: Just calls `Serialize()` on the const reference
- **Minimal aliasing risk**: Buffer is copied into packet immediately

### Code Pattern
```cpp
std::vector<char> Buffer;
const double TimestampSeconds = FPlatformTime::Seconds();

int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);
// ... copy buffer into packet ...
Channel->Queue.Enqueue(MoveTemp(Packet));
```

**Key Point**: No modification to the received SubjectList structure. Just read and serialize.

---

## 2. SOCKETS TCP TRANSPORT

**File**: `Open3DTransportSockets/Private/Sender/SocketsTcpSender.cpp:151-187`

### Design Pattern
- **Serializes entire SubjectList as-is** (all subjects together)
- **Reuses serialization buffer** across frames to avoid malloc/free overhead
- Uses member variable `SerializationScratch` as pre-allocated buffer

### Send() Flow
```
Input: SubjectList (may contain multiple subjects)
  ↓
Clear scratch buffer (reuse)
  ↓
Serialize(List) → single buffer in SerializationScratch
  ↓
Enqueue payload from scratch buffer
  ↓
Return (List still valid)
```

### Ownership Management
- **No ownership transfer**: The incoming `List` is read-only
- **Buffer reuse**: `SerializationScratch` is a member variable cleared/reused
- **Memory optimization**: Avoids repeated allocations at 60fps

### Code Pattern
```cpp
SerializationScratch.clear();
int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(SerializationScratch, Timestamp);

if (!EnqueuePayload(reinterpret_cast<const uint8*>(SerializationScratch.data()), BytesWritten))
{
    // handle error
}
```

**Key Point**: Same philosophy as Loopback—serialize as-is, but with performance optimization.

---

## 3. NNG TRANSPORT

**File**: `Open3DTransportNNG/Private/Sender/NngSender.cpp:254-289`

### Design Pattern
- **Serializes entire SubjectList as-is** (all subjects together)
- **Creates temporary buffer** for each frame (no reuse)
- Extracts first subject name for metadata

### Send() Flow
```
Input: SubjectList (may contain multiple subjects)
  ↓
Create temporary buffer
  ↓
Serialize(List) → single buffer (all subjects)
  ↓
Extract metadata (first subject name)
  ↓
Enqueue payload
  ↓
Return (List still valid)
```

### Ownership Management
- **No ownership transfer**: The incoming `List` is read-only
- **Temporary buffer**: Created fresh each frame, destroyed after `EnqueuePayload()`
- **Simple, straightforward**: No clever buffer reuse

### Code Pattern
```cpp
std::vector<char> Buffer;
const double TimestampSeconds = FPlatformTime::Seconds();
int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);

if (!EnqueuePayload(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten))
{
    // handle error
}
```

**Key Point**: Identical to Loopback—serialize the whole SubjectList, no decomposition.

---

## 4. WebRTC TRANSPORT (THE OUTLIER)

**File**: `Open3DTransportWebRTC/Private/Sender/WebRTCSender.cpp:411-552`

### Design Pattern (UNIQUE)
- **Decomposes SubjectList into per-subject packets**
- **Creates a temporary single-subject SubjectList for each subject**
- **Sends each subject on its own labeled data channel**
- **Copies Transform pointers** from source to temporary (DANGEROUS!)

### Send() Flow
```
Input: SubjectList (may contain multiple subjects)
  ↓
For each subject in List:
  ├─ Create temporary SingleSubjectList
  ├─ Add subject data and Transform pointers to temp
  ├─ Serialize temp → buffer for THIS subject
  ├─ Send buffer on labeled channel: "subject_name"
  └─ [BUG WAS HERE] SingleSubjectList destroyed → tries to delete Transforms
  ↓
Return
```

### What Makes WebRTC Different?

**All other transports**:
- Serialize the entire multi-subject list as a single blob
- Send it as-is to a single channel/connection
- No decomposition or reconstruction needed

**WebRTC**:
- Must send each subject on its own **labeled data channel**
- LiveKit FFI requires `lk_send_data_ex(ClientHandle, buffer, size, ..., label)`
- The label is the subject name (e.g., "Armature", "CameraRig")
- Receiver can subscribe to specific subjects by label

### Why the Per-Subject Decomposition?

**Architecture Goal**: Enable **selective subscription** on the receiver side

```
Sender (WebRTC):
  Armature ──[labeled channel: "Armature"]──> Network
  CameraRig ─[labeled channel: "CameraRig"]──> Network

Receiver:
  "I want Armature" → Subscribe to "Armature" channel only
  "I want CameraRig" → Subscribe to "CameraRig" channel only
  "I want both" → Subscribe to both channels
```

This is **fundamentally different** from the other transports:
- **Loopback/Sockets/NNG**: Single stream with all subjects (receiver must parse/filter)
- **WebRTC**: Multiple labeled streams (receiver can filter at transport level)

### The Ownership Bug

Because WebRTC creates temporary `SingleSubjectList` objects:

```cpp
O3DS::SubjectList SingleSubjectList;  // Stack variable
O3DS::Subject* NewSubject = SingleSubjectList.addSubject(Subject->mName, Subject->mReference);

// Copy Transform POINTERS (not copies!)
for (const auto& Transform : Subject->mTransforms.mItems)
{
    if (Transform)
    {
        NewSubject->addTransform(Transform);  // Adds raw pointer!
    }
}

// Serialize the buffer (this works, references are still valid)
int32 BytesWritten = SingleSubjectList.Serialize(Buffer, TimestampSeconds);

// NOW SingleSubjectList goes out of scope...
// Its destructor calls ~SubjectList() → ~Subject() → ~TransformList()
// ~TransformList() tries to DELETE the Transform pointers
// But those pointers are STILL OWNED by the original incoming List!
```

When the original `List` is destroyed later, it tries to delete those same Transform objects **again** → **double-delete crash**.

### The Fix

Clear the Transform list before the temporary is destroyed:

```cpp
// Serialize (buffer now contains serialized data, not references)
int32 BytesWritten = SingleSubjectList.Serialize(Buffer, TimestampSeconds);

// IMPORTANT: Clear the transform list before SingleSubjectList is destroyed.
// We added raw pointers from the source Subject, so we must prevent
// SingleSubjectList's destructor from deleting them (they're still owned by List).
NewSubject->mTransforms.mItems.clear();

// Now it's safe for SingleSubjectList to be destroyed
// Its destructor will try to delete nothing
```

---

## Summary Table

| Aspect | Loopback | Sockets | NNG | WebRTC |
|--------|----------|---------|-----|--------|
| **Serialization** | Multi-subject blob | Multi-subject blob | Multi-subject blob | Per-subject decomposition |
| **Channel Model** | Single queue | Single TCP stream | Single NNG socket | Multiple labeled channels |
| **Temporary Objects** | None | None | None | **One per subject** |
| **Ownership Risk** | None | None | None | **HIGH** (copies pointers) |
| **Receiver Filtering** | Post-parse | Post-parse | Post-parse | **Transport-level** |
| **Why Different** | Simpler protocol | Performance | Pub/sub pattern | LiveKit FFI design |

---

## Design Implications

### WebRTC's Architectural Choice

WebRTC chose per-subject labeled channels because:

1. **LiveKit FFI Design**: The underlying C API (`lk_send_data_ex()`) expects labeled messages
2. **Receiver Efficiency**: Subscribers can filter at transport level instead of parsing
3. **Scalability**: Multiple subjects don't compete for a single channel's bandwidth
4. **Real-time Priority**: Critical subjects can be prioritized without parsing overhead

### Trade-off: Complexity vs. Functionality

- **Gain**: Better receiver-side filtering, true per-subject audio tracks per-subject data channels
- **Cost**: Must manage temporary objects with borrowed ownership, risk of double-deletion if not careful

### Why Other Transports Don't Have This Issue

- **Loopback**: Test harness, single process, memory management less critical
- **Sockets**: Simple streaming protocol, no concept of labeled channels
- **NNG**: Pub/sub network pattern, but uses multi-subject serialization (no decomposition)

---

## Lessons Learned

1. **Borrowed Ownership is Dangerous**: Copying raw pointers between objects with different lifetimes is error-prone
2. **Transport-Specific Requirements**: Protocol designs force implementation differences; not all transports are equal
3. **Serialization Semantics Matter**: What gets serialized (whole list vs. single subject) affects object ownership
4. **Fix is Simple but Subtle**: Clearing the pointer list before destruction is correct, but requires understanding the design

