# WebRTC Connector Quickstart (LibDataChannel + UE)

This guide shows how to smoke-test the new backend-agnostic WebRTC connector against the libdatachannel sample signaling server.

## Prerequisites

- Unreal Engine 5.6 (same as ProjectSandbox)
- Python 3.x
- The libdatachannel sample signaling server (`plugins/unreal/Open3DStream/examples/signaling/signaling-server.py`)

> Note: The sample server uses WebSockets and a simple JSON schema (type = offer/answer/candidate, plus `id`, `description`, `mid`, etc.).

## 1) Start the signaling server

From a terminal:

```powershell
# In repo root
cd plugins/unreal/Open3DStream/examples/signaling
python .\signaling-server.py
```

 By default this listens on `ws://127.0.0.1:8080`. Each peer connects to `ws://host:port/<localId>` and sends messages addressed to a remote peer via the `id` field in JSON.

## 2) Add test components in ProjectSandbox

In the UE editor (ProjectSandbox):

  - SignalingUrl: `ws://127.0.0.1:8000`
  - SignalingUrl: `ws://127.0.0.1:8080`
  - LocalId: `server`
  - bAppendLocalIdToUrl: true (default)
  - bServer: true
  - Room: (leave empty)
  - bEnableAudio: false (optional)
   - bEnableAudio: false (optional)
   - bSendDebugTone: true (optional) to synthesize a 440 Hz tone for ~1s and send it as RTP when audio opens
  - `SignalingConnected` for both components
  - `PeerConnectionState:*` transitions
  - `DataChannelOpen` on both sides
  - The client sends a one-time message "hello from example component" when the DC opens; the server prints it under `[ExampleConnector] Data: ...`

If `bEnableAudio` is true, the SDP will include audio m-lines (client=sendonly, server=recvonly). This example does not generate audio packets; receiving RTP can be observed when the sending peer actually publishes audio.

## Notes

  - `Source/Open3DBroadcast/Public/O3DSWebRTCConnectorComponent.h`
  - `Source/Open3DBroadcast/Private/O3DSWebRTCConnectorComponent.cpp`
