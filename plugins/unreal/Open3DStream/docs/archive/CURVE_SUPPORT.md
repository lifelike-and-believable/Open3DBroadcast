# Open3DStream Animation Curve Support

This update adds support for animation curve data to the Open3DStream protocol and LiveLink plugin, enabling morph target-based facial animation alongside skeletal animation data.

## Changes Made

### 1. Protocol Schema Updates (`src/o3ds.fbs`)
- Added `Curve` table with `name:string` and `value:float` fields
- Added `CurveUpdate` struct with `value:float` and `i:int` fields  
- Added `curves:[Curve]` field to `Subject` table
- Added `curves:[CurveUpdate]` field to `SubjectUpdate` table

### 2. Core Library Updates (`src/o3ds/model.h` and `src/o3ds/model.cpp`)
- Added `mCurveNames` and `mCurveValues` vectors to `Subject` class
- Implemented `Subject::SerializeCurves()` method to serialize curve data
- Implemented `Subject::SerializeCurveUpdates()` method for delta updates
- Updated `Subject::Serialize()` to include curve data
- Updated `Subject::SerializeUpdate()` to include curve updates
- Updated `SubjectList::ParseSubject()` to read curve data from incoming packets
- Updated `SubjectList::ParseUpdate()` to process curve value updates

### 3. Unreal LiveLink Plugin Updates (`plugins/unreal/Open3DStream/Source/Open3DStream/Private/Open3DStreamSource.cpp`)
- Added code to populate `FrameData.CurveNames` and `FrameData.CurveValues` from subject curve data
- Curves are now delivered through the standard LiveLink animation pipeline

## Usage

### Setting Curve Data (Sender Side)
```cpp
O3DS::SubjectList subjects;
auto subject = subjects.addSubject("Character");

// Add curve names and values
subject->mCurveNames.push_back("EyeBrowUp_L");
subject->mCurveNames.push_back("EyeBrowUp_R"); 
subject->mCurveNames.push_back("Smile");

subject->mCurveValues.push_back(0.5f);  // EyeBrowUp_L = 50%
subject->mCurveValues.push_back(0.3f);  // EyeBrowUp_R = 30%
subject->mCurveValues.push_back(0.8f);  // Smile = 80%

// Serialize and send
std::vector<char> buffer;
subjects.Serialize(buffer);
// ... send buffer via network
```

### Updating Curve Values
```cpp
// Update individual curve values
subject->mCurveValues[0] = 0.7f;  // EyeBrowUp_L = 70%
subject->mCurveValues[2] = 0.2f;  // Smile = 20%

// Send incremental update
size_t count = 0;
subjects.SerializeUpdate(buffer, count);
// ... send buffer via network
```

### Receiving in Unreal (LiveLink)
The curves are automatically populated in `FLiveLinkAnimationFrameData`:
- `FrameData.CurveNames` contains the curve names as `FName` objects
- `FrameData.CurveValues` contains the corresponding float values
- These integrate with Unreal's morph target system through LiveLink

## Building

After making changes to the schema:
1. Regenerate the FlatBuffers header: `flatc --cpp src/o3ds.fbs`
2. Build the core library
3. Build the Unreal plugin

## Compatibility

- Maintains backward compatibility with existing skeletal animation data
- Curve data is optional - subjects without curves work normally
- New curve fields are ignored by older versions of the protocol

## Technical Notes

- Curves are stored at the subject level, not per-transform
- Curve names should match morph target names in the target application
- Curve values are typically in the range 0.0-1.0 but this is not enforced
- Curve updates support delta optimization like transform updates
- The protocol supports an unlimited number of curves per subject