# Open3DBroadcast

> Lightweight, standardized live streaming of 3D animation

Open3DBroadcast streams skeletal animation and animation curves (morph targets) between engines and tools in real time. Open3DStream is the name of the wire protocol and of the C++ core library that implement it. This repository holds:

- **The Open3DStream core library** (`src/o3ds`): the FlatBuffers data model, serialization with delta updates, sequencing, reordering and prediction, and TCP, UDP and NNG connectors.
- **Open3DBroadcast**, the Unreal Engine plugin (`ProjectSandbox/Plugins/Open3DBroadcast`): sends and receives Open3DStream data between Unreal instances over pluggable transports, with LiveLink on the receiving side.
- **Open3DBroadcastWebRTC**, a free add-on plugin (`ProjectSandbox/Plugins/Open3DBroadcastWebRTC`) that adds a WebRTC (LiveKit) transport.
- **ProjectSandbox**, the Unreal project used to develop and test both plugins.
- Command-line apps (`apps/`).

## Unreal Engine plugins

| | Open3DBroadcast | Open3DBroadcastWebRTC |
|---|---|---|
| Engine | Unreal Engine 5.7 only | Unreal Engine 5.7 only |
| Platform | Win64 (Editor, Game and Client targets) | Win64 |
| Status | Beta; the MoQ transport is Experimental | Beta |
| Transports | Loopback, Sockets (TCP/UDP), NNG, MoQ (draft-ietf-moq-transport-07) | WebRTC through LiveKit |
| Docs | [README](ProjectSandbox/Plugins/Open3DBroadcast/README.md), [User Guide](ProjectSandbox/Plugins/Open3DBroadcast/USER_GUIDE.md), [Transport comparison](ProjectSandbox/Plugins/Open3DBroadcast/Transport_Module_Comparison.md) | [README](ProjectSandbox/Plugins/Open3DBroadcastWebRTC/README.md), [User Guide](ProjectSandbox/Plugins/Open3DBroadcastWebRTC/USER_GUIDE.md) |

The add-on depends on Open3DBroadcast and is installed next to it (ADR 0002).

What the plugins provide:

- **Sender:** `UO3DSenderComponent` captures a skeletal mesh's pose and animation curves and streams them under a subject name. `UO3DSenderAudioCaptureComponent` sends audio with it.
- **Receiver:** a LiveLink source, **Open3D Receiver Source**, that exposes each received subject as a LiveLink subject. `UO3DRemoteAudioComponent` plays received audio.
- **Editor:** Details panels for the sender and the transport settings, and the LiveLink "Add Source" panel.

### Install from source

1. Copy `ProjectSandbox/Plugins/Open3DBroadcast` (and `Open3DBroadcastWebRTC` if you want WebRTC) into your project's `Plugins/` folder.
2. Enable the plugins in **Edit → Plugins** and build your editor target.

No CMake or pre-build step is needed. The plugin compiles the o3ds core from a copy in its `Source/` folder, and its prebuilt third-party libraries (Opus, NNG, moq-ffi; livekit_ffi in the add-on) are committed there too.

### First stream

The [User Guide quick start](ProjectSandbox/Plugins/Open3DBroadcast/USER_GUIDE.md#quick-start) walks through a Loopback stream in one editor:

1. Add an **O3D Sender Component** to an actor with a skeletal mesh, set a **Subject Name** and set **Transport Name** to `loopback`.
2. In **Window → Live Link**, click **+ Source → Open3D Receiver Source** with the same transport, then **Create**.
3. Press Play. The subject appears in the LiveLink panel, ready for a Live Link Component on another character.

## Building and running ProjectSandbox locally

ProjectSandbox contains both plugins in-tree and enables them. On Windows with UE 5.7, from the repository root and with the editor closed:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Build\BatchFiles\Build.bat" ProjectSandboxEditor Win64 Development "-Project=$PWD\ProjectSandbox\ProjectSandbox.uproject" -WaitMutex
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe" "$PWD\ProjectSandbox\ProjectSandbox.uproject"
```

You can also open `ProjectSandbox/ProjectSandbox.uproject` in Rider or Visual Studio and build `ProjectSandboxEditor`.

For packaging (`RunUAT BuildPlugin`, the Fab source zip), automation tests, build flags and CI, see [Build/README.md](Build/README.md).

## Core library (C++)

The core's source of truth for the data model is the FlatBuffers schema `src/o3ds.fbs`. `src/o3ds_generated.h` is generated from it with `flatc --cpp -o src src/o3ds.fbs` and checked in.

### Building

Initialize the submodules:

```bash
git submodule update --init --recursive
```

The core finds FlatBuffers and NNG through CMake packages, so install the third-party libraries to a prefix first, then build the core against it. This is what the `O3DS linux` and `O3DS windows` workflows do (`.github/workflows/linux.yml`, `windows.yml`):

```bash
PREFIX=$PWD/usr
for dep in nng cml crccpp flatbuffers; do
  cmake -S thirdparty/$dep -B thirdparty/$dep/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=$PREFIX
  cmake --build thirdparty/$dep/build --config Release
  cmake --install thirdparty/$dep/build --config Release
done

cmake -S . -B build -DCMAKE_PREFIX_PATH=$PREFIX -DO3DS_DISABLE_WEBRTC=ON
cmake --build build --config Release
```

- The core library is `open3dstreamstatic`: the model, wire format, sequencing, prediction and the other modules the Unreal plugin compiles. It needs only FlatBuffers.
- The legacy connectors (`Connector`/`AsyncConnector`: TCP, UDP, NNG pub/sub/pair/pipeline/request, the libdatachannel WebRTC connector, the XSens parser) are a separate library, `open3dstream_legacy`, built only with `-DO3DS_BUILD_LEGACY=ON` (CORE-27). Nothing in this repository uses them; the Unreal plugin has its own transports. Its WebRTC connector (option `O3DS_ENABLE_WEBRTC`) needs libdatachannel, which is no longer in the tree: add `-DO3DS_DISABLE_WEBRTC=ON`, or point `O3DS_LIBDATACHANNEL_ROOT` at a prebuilt libdatachannel. `-DO3DS_DISABLE_WEBRTC=ON` in the command above only matters with the legacy library.
- `O3DS_BUILD_TESTS` (on by default) builds the CTest suite. `O3DS_ENABLE_SANITIZERS` (GCC/Clang) and `O3DS_BUILD_FUZZERS` (Clang) enable ASan/UBSan and the libFuzzer targets. `core-tests.yml` runs all three.

### Example: building a frame

```cpp
#include "o3ds/model.h"

O3DS::SubjectList subjects;
O3DS::Subject* subject = subjects.addSubject("Character");

// Skeleton
O3DS::Transform* root = subject->addTransform("Root", -1);
root->translation.value = O3DS::Vector3d(1.0, 0.0, 0.0);

// Facial curves
subject->mCurveNames  = { "Smile", "EyeBrowUp_L", "EyeBrowUp_R" };
subject->mCurveValues = { 0.8f, 0.5f, 0.3f };

// Serialize a full frame; send `buffer` with any connector
std::vector<char> buffer;
subjects.Serialize(buffer, /*timestamp*/ 0.0);
```

`SubjectList::SerializeUpdate` produces delta updates against the last frame sent.

### TCP framing

Each TCP message is an 18-byte header followed by the FlatBuffers payload: the 14-byte magic `00 FF 03 FE "O3DS-START"`, then the payload length as a little-endian `uint32` (`src/o3ds/tcp_stream_parser.*`).

```python
import socket, struct

MAGIC = b"\x00\xff\x03\xfeO3DS-START"

with socket.create_connection(("127.0.0.1", 5555)) as s:
    header = s.recv(18, socket.MSG_WAITALL)
    if header[:14] != MAGIC:
        raise SystemExit("Invalid stream header")
    (size,) = struct.unpack("<I", header[14:18])
    payload = s.recv(size, socket.MSG_WAITALL)
    print(f"Received {len(payload)} bytes of O3DS data")
```

## Apps

- `apps/Repeater`: relays a stream from one address to another. A Docker image is built from `docker/Dockerfile.repeater`:

  ```bash
  docker build -f docker/Dockerfile.repeater -t open3dstream-repeater:local .
  docker run --rm -p 7000:7000 -p 7001:7001 open3dstream-repeater:local
  ```

- `apps/DeterminismProbe`, `apps/PredictorEval` and `apps/QuantEval` are measurement tools for the roadmap's prediction and quantization work, built by the root `CMakeLists.txt`.
- `apps/FbxStream`, `apps/Test1`, `apps/SubscribeTest` and `apps/XSensTest` were archived in 2026-10 (WP-A7, CORE-28). They are kept, unchanged, at the git tag `archive/legacy-apps`; they used the legacy connectors.
- The MotionBuilder and Maya plugins (`plugins/`), the Python scripts (`python/`) and the Sphinx documentation site (`sphinx/`) were archived in 2026-10 (WP-A7). They are kept, unchanged, at the git tag `archive/dcc-plugins-python-sphinx`.

## Documentation

- [Build/README.md](Build/README.md): build scripts, packaging, tests and CI
- [docs/adr/](docs/adr/README.md): architecture decision records
- [docs/roadmap/](docs/roadmap/): roadmaps
- [docs/testing/webrtc-manual-test.md](docs/testing/webrtc-manual-test.md): manual WebRTC test
- [CHANGELOG.md](CHANGELOG.md)

## Contributing

Contributions are welcome through pull requests against `develop`. CI runs the core tests on every PR. Non-draft PRs that touch the plugins also get the UE build and automation tests; [Build/README.md](Build/README.md#cicd-integration) lists what each job checks.

- **Community:** [Discord](https://discord.gg/UdEbyFM9wD)
- **Issues:** [GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)

## License

MIT. See [LICENSE](LICENSE). Third-party licences for the plugin are in [THIRD_PARTY_LICENSES.md](ProjectSandbox/Plugins/Open3DBroadcast/THIRD_PARTY_LICENSES.md).

## Credits

Created by Alastair Macleod (<http://mocap.ca/>).
