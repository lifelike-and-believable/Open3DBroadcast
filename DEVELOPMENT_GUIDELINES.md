# Development Guidelines

## Critical Rules

### 1. Always Read Header Files Before Using APIs
**Rule:** Never guess or assume API signatures, member names, or class structures. Always read the actual header files first.

**Why:** Using incorrect API names or non-existent members causes compilation failures and wasted build time.

**Example of WRONG approach:**
```cpp
// WRONG: Guessing API
for (int i = 0; i < List.size(); i++)
  const auto& item = List.at(i);  // At() doesn't exist!
  NewList.add();                   // add() doesn't exist!
  *NewList.mutable_items(0) = item; // mutable_items() doesn't exist!
```

**Example of CORRECT approach:**
```cpp
// CORRECT: Check header first
// From model.h, confirmed:
// - SubjectList has public member: std::vector<Subject*> mItems
// - SubjectList has method: size_t size() { return mItems.size(); }
// - SubjectList has method: Subject* operator[] (int ref) { return mItems.operator[](ref); }
// - Subject has public members: mName, mReference, mTransforms, mJoints, mCurveNames, mCurveValues
// - Subject has method: void addTransform(Transform* item)

for (size_t i = 0; i < List.mItems.size(); ++i)
{
    O3DS::Subject* Subject = List.mItems[i];
    if (Subject)
    {
        NewList.addSubject(Subject->mName, Subject->mReference);
        // ... copy data
    }
}
```

### 2. Check All Third-Party Header Files
**Files to always consult before writing code:**
- `open3dstream/include/o3ds/model.h` - Subject, SubjectList, Transform APIs
- `livekit_ffi/include/livekit_ffi.h` - LiveKit FFI function signatures
- Unreal Engine header files - Data types, container APIs

### 3. UE 5.7 Build Configuration
**Always use these settings:**
- `.uproject` file: `"EngineAssociation": "5.7"`
- `.Target.cs` files:
  ```cpp
  DefaultBuildSettings = BuildSettingsVersion.V6;
  IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
  ```

**Why:** Ensures consistent builds and prevents version-related compilation errors.

### 4. Build Process
When implementing new code:
1. Check header files for actual API signatures
2. Write code matching the real API
3. Build with UE 5.7
4. Fix any compilation errors immediately
5. Commit with proper messages

### 5. Common Header File Locations
```
- Open3DStream Data: ProjectSandbox/Plugins/Open3DBroadcast/ThirdParty/open3dstream/include/o3ds/model.h
- LiveKit FFI: ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportWebRTC/ThirdParty/livekit_ffi/include/livekit_ffi.h
```

---

## Recent Build Fix Examples

### O3DS::SubjectList API (CORRECT)
```cpp
class SubjectList
{
public:
    std::vector<Subject*> mItems;  // Direct access
    size_t size() { return mItems.size(); }  // Get count
    Subject* operator[] (int ref) { return mItems.operator[](ref); }  // Index access
    Subject* addSubject(std::string name, void *info=nullptr);  // Add subject
};
```

### O3DS::Subject API (CORRECT)
```cpp
class Subject
{
public:
    std::string mName;
    std::vector<std::string> mJoints;
    std::vector<std::string> mCurveNames;
    std::vector<float> mCurveValues;
    TransformList mTransforms;
    void* mReference;
    Context mContext;

    Transform* addTransform(const std::string& name, int parentId, TransformBuilder *builder = nullptr);
    void addTransform(Transform* item);  // Add existing transform
};
```

### Correct Usage Pattern
```cpp
// Iterate over subjects
for (size_t idx = 0; idx < List.mItems.size(); ++idx)
{
    O3DS::Subject* Subject = List.mItems[idx];
    if (!Subject) continue;

    // Create new subject
    O3DS::SubjectList NewList;
    O3DS::Subject* NewSubject = NewList.addSubject(Subject->mName, Subject->mReference);

    // Copy data using actual member names
    NewSubject->mJoints = Subject->mJoints;
    NewSubject->mCurveNames = Subject->mCurveNames;
    NewSubject->mCurveValues = Subject->mCurveValues;

    // Copy transforms
    for (const auto& Transform : Subject->mTransforms.mItems)
    {
        if (Transform)
            NewSubject->addTransform(Transform);
    }
}
```

---

## Lesson Learned
The build errors on November 18, 2025 were caused by:
1. Assuming O3DS API existed without checking model.h
2. Guessing member names (mRef, mFrameNumber, mQuatRotation, mTranslation)
3. Guessing method names (at(), add(), mutable_subjects())

**Prevention:** Always read the actual header files first. No guessing.

---

**Effective Date:** November 18, 2025
**Updated By:** Claude Code
