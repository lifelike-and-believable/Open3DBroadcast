# OPEN3DSTREAM FRAME SERIALIZATION - TECHNICAL DEEP DIVE

## Mocap Serialization (SubjectList)

Location: src/o3ds/model.cpp, src/o3ds/model.fbs

FlatBuffer Schema:
- SubjectList contains array of Subjects
- Each Subject contains array of Transforms
- Each Transform contains: parent ID, name, translation (12 bytes), rotation (16 bytes), scale (12 bytes)
- Optional matrices (64 bytes each), component order, curves

Wire Frame Structure:
[Flags: 4B] [CRC32: 4B] [FlatBuffer data: variable]

Per-Transform Serialization:
- Translation: 3 floats = 12 bytes
- Rotation: 4 floats = 16 bytes
- Scale: 3 floats = 12 bytes
- Name string: variable (5-20 bytes typical)
- Parent ID: 4 bytes
- Component flags: 1-4 bytes

Overhead per transform:
- FlatBuffer vtables: 8-10 bytes
- String offsets: 4 bytes
- Type info/padding: 10-20 bytes


## Audio Serialization (Unified Message)

Unified Header (20 bytes):
- Magic: 4 bytes (0x4F334441)
- Version: 1 byte
- Kind: 1 byte (0=Mocap, 1=Audio)
- Codec: 1 byte (0=O3DS, 1=Opus, 2=PCM16)
- Flags: 1 byte
- Timestamp: 8 bytes (microseconds, big-endian)
- PayloadSize: 4 bytes (big-endian)

PCM16 Payload (40 byte header):
- Version: 1 byte
- Flags: 1 byte
- Channels: 2 bytes (uint16LE)
- SampleRate: 4 bytes (uint32LE)
- Timestamp: 8 bytes (doubleLE)
- SourceGuid: 16 bytes (4x uint32LE)
- LabelSize: 2 bytes (uint16LE)
- SubjectSize: 2 bytes (uint16LE)
- PCMBytes: 4 bytes (uint32LE)
- StreamLabel: variable (UTF-8)
- SubjectName: variable (UTF-8)
- PCM16 Data: variable

Encoded Payload (42 byte header):
- Version: 1 byte
- Flags: 1 byte
- Codec: 1 byte
- Reserved: 1 byte
- Same as above with codec field


## Frame Size Calculations

MOCAP (50-bone):
- Wire header: 8 bytes
- Subject overhead: 66 bytes
- Per transform x50: 50 x 77 = 3,850 bytes
- FlatBuffer padding: 200 bytes
- TOTAL: ~4,120 bytes

AUDIO PCM16 (48kHz, stereo, 20ms):
- Unified header: 20 bytes
- Payload header: 40 bytes
- Strings: 18 bytes
- PCM data: 960 samples x 2 channels x 2 = 3,840 bytes
- TOTAL: 3,918 bytes

AUDIO OPUS 64kbps (20ms):
- Unified header: 20 bytes
- Payload header: 42 bytes
- Strings: 18 bytes
- Encoded: 160 bytes
- TOTAL: 240 bytes


## Size Determinants

MOCAP (in order of impact):
1. Number of bones (PRIMARY): ~70 bytes per bone
2. Bone name lengths (SECONDARY): ~12 bytes avg
3. Subject metadata (TERTIARY): ~50 bytes
4. Curves (if present): ~20-50 bytes each
5. Custom matrices: 64 bytes each
6. FlatBuffer overhead: 400-600 bytes fixed

AUDIO (in order of impact):
1. Codec selection: PCM16 vs Opus = 10x difference
2. Sample rate: Linear scaling
3. Channels: Linear scaling (mono vs stereo vs surround)
4. Frame duration: Linear
5. String metadata: Negligible (<0.5%)


## Actual Code Implementations

Subject::Serialize() in model.cpp:320-381:
- Creates FlatBuffer for each Transform
- Packs translation, rotation, scale structs
- Creates component order array
- Returns FlatBuffer offset

SubjectList::Serialize() in model.cpp:507-530:
- Iterates through all subjects
- Serializes each subject individually
- Creates root SubjectList table
- Calls finalize() to add CRC/flags wrapper
- Returns total serialized size

SerializePcm16Frame() in O3DAudioSerialization.cpp:87-139:
- Writes 40-byte header with metadata
- UTF-8 encodes strings
- Memcpy's PCM16 data directly
- Returns total size

SerializeEncodedAudioFrame() in O3DAudioSerialization.cpp:219-278:
- Writes 42-byte header (includes codec field)
- UTF-8 encodes strings
- Copies pre-encoded bitstream
- Returns total size


## Network Implications

UDP (1500 byte MTU):
- Mocap 4KB: 3 packets per frame
- PCM16 3.9KB: 3 packets per frame
- Opus 240B: 1 packet per frame

TCP/WebRTC:
- No MTU limit
- Frame assembly automatic
- Frame latency = size / bitrate


## Optimization Techniques (Current)

Already implemented:
- SubjectUpdate for delta compression
- Opus codec for audio
- Variable bitrate encoding
- FlatBuffer efficiency

Potential (not implemented):
- Bone subsets (50% mocap reduction)
- Reduced precision (50% mocap reduction)
- String deduplication (10% mocap reduction)
- Selective transform updates (90% mocap reduction in steady state)
