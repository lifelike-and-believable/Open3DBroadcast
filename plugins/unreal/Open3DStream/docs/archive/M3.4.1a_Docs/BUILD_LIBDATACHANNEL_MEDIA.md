# Building libdatachannel with Media (Opus) for UE

To enable native WebRTC audio tracks in P2P mode, build libdatachannel with media and Opus support and package it for the Unreal plugin.

## Configure and build

Windows (MSVC):
```bash
cmake -S . -B build -A x64 ^
  -DUSE_MEDIA=ON -DUSE_OPUS=ON -DUSE_MBEDTLS=ON ^
  -DBUILD_SHARED_LIBS=OFF ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
cmake --build build --config Release
```

macOS:
```bash
cmake -S . -B build \
  -DUSE_MEDIA=ON -DUSE_OPUS=ON -DUSE_MBEDTLS=ON \
  -DBUILD_SHARED_LIBS=OFF
cmake --build build --config Release
```

Linux:
```bash
cmake -S . -B build \
  -DUSE_MEDIA=ON -DUSE_OPUS=ON -DUSE_MBEDTLS=ON \
  -DBUILD_SHARED_LIBS=OFF
cmake --build build -j
```

## Deploy to plugin

- Place headers and libs under the plugin’s ThirdParty/libdatachannel/<platform>/include and /lib
- Ensure Open3DStream.Build.cs and/or Open3DBroadcast.Build.cs link against the rebuilt libs
- Verify O3DS_ENABLE_WEBRTC is defined so WebRTC UI appears
- On Start(), SDP should contain `m=audio` with Opus

## Sanity checks

- Example peer (browser) receives audio when sender is enabled
- Latency ~100–200 ms typical on local/STUN paths
- No Game Thread stalls; audio encode/push runs on worker threads