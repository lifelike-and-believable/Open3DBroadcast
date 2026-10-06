# Open3DBroadcast Plugin - User Guide

## Table of Contents

1. [Introduction](#introduction)
2. [Quick Start](#quick-start)
3. [Core Concepts](#core-concepts)
4. [Sender Setup](#sender-setup)
5. [Receiver Setup](#receiver-setup)
6. [Transport Modules](#transport-modules)
7. [Audio Streaming](#audio-streaming)
8. [Control Channel](#control-channel)
9. [LiveLink Integration](#livelink-integration)
10. [Configuration Reference](#configuration-reference)
11. [Performance Tuning](#performance-tuning)
12. [Troubleshooting](#troubleshooting)
13. [Advanced Topics](#advanced-topics)

---

## Introduction

The **Open3DBroadcast Plugin** is a comprehensive Unreal Engine plugin for streaming skeletal animation data and audio in real-time. It enables you to capture motion from skeletal meshes in one Unreal Engine instance and receive it in another (or the same) instance using various network transports.

### Requirements and Status

- **Unreal Engine:** 5.7. Other engine versions are not supported.
- **Platform:** Win64 (Windows 64-bit) only, for editor and game targets. Server and Program targets are not supported.
- **Status:** Beta. The MoQ transport (`Open3DTransportMoQ`) is Experimental: it implements draft-ietf-moq-transport-07, MoQ relays must speak draft-07, and its options and behaviour can change between releases.
- **WebRTC (LiveKit):** not part of this plugin. It is available as a free add-on plugin, **Open3DBroadcastWebRTC**, that you install next to Open3DBroadcast. Download: **[DOWNLOAD LINK PLACEHOLDER: support-site URL for Open3DBroadcastWebRTC, to be added before release]**. See [WebRTC Transport (free add-on)](#webrtc-transport-free-add-on).
- **Support:** [GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)

### Key Features

- **Real-time skeletal animation streaming** at configurable frame rates
- **Audio capture and streaming** from game audio or microphone
- **Multiple transport options**: Loopback (testing), Sockets (TCP/UDP), NNG (pub/sub), MoQ (Experimental); WebRTC (cloud-ready) with the free Open3DBroadcastWebRTC add-on
- **LiveLink integration** for seamless animation retargeting
- **Curve and morph target support** for facial animation
- **Production-ready** with comprehensive error handling and statistics

### Use Cases

- **Virtual production**: Stream motion capture data to rendering workstations
- **Remote collaboration**: Share character animations across the internet
- **Multi-machine setups**: Distribute animation processing across multiple systems
- **Development/Testing**: Test animation pipelines locally with loopback transport

---

## Quick Start

### Installation

1. Install the plugin: from Fab, install it to your engine with the Epic Games Launcher; from a download, copy the `Open3DBroadcast` folder into your project's `Plugins/` folder.
2. Open your project, go to **Edit → Plugins** and search for "Open3D".
3. Enable **Open3DBroadcast** (it is marked Beta). Unreal's **Live Link** plugin is enabled with it, because Open3DBroadcast depends on it.
4. Restart the editor when prompted.

### Your First Stream

This example streams the Third Person template's mannequin to a second mannequin in the same level, over the **Loopback** transport (in-process, no network). No sample map ships with the plugin, so it starts from a new project.

#### Step 1: Create a Project with a Mannequin

1. Create a new project from the **Third Person** template (**Games** category), Blueprint or C++.
2. Enable the plugin as in [Installation](#installation) and restart the editor.

#### Step 2: Add a Sender to the Character

1. In the **Content Browser**, open the template's character Blueprint, `BP_ThirdPersonCharacter` (in `Content/ThirdPerson/Blueprints`).
2. In the **Components** panel, click **Add** and add an **O3D Sender** component (search for "O3D Sender").
3. Select the new component. In the **Details** panel, under **Open3DBroadcast**:
   - **Sender** group: set **Subject Name** to `Mannequin`. Leave **Auto Start Capture** ticked (the default) and **Target Mesh** empty: the component captures the character's skeletal mesh.
   - **Transport** group: tick **Auto Create Transport**. The warning "Auto Create Transport is off ..." disappears and **Transport Name** becomes editable.
   - **Transport Name**: **Loopback** (the default). Leave **Channel Name** empty: both ends then use the channel `default`.
4. **Compile** and **Save** the Blueprint.

#### Step 3: Create the LiveLink Source (Receiver)

1. Open **Window → Virtual Production → Live Link**.
2. Click **Add Source** and choose **Open3DStream Receiver**.
3. In the panel that opens, set **Transport** to **Loopback** (the default) and leave **Channel Name** empty.
4. Click **Create Source**. The source appears in the Live Link panel and waits for data.

#### Step 4: Make an Animation Blueprint that Reads the Subject

1. In the **Content Browser**, right-click and choose **Animation → Animation Blueprint**. Pick the mannequin's skeleton (`SK_Mannequin` in the template) and name the asset, for example `ABP_O3DMannequin`.
2. Open it. In the **AnimGraph**, right-click, add a **Live Link Pose** node and connect its output to **Output Pose**.
3. On the node, set **Live Link Subject Name** to `Mannequin`: type it, or pick it from the list once the subject exists.
4. **Compile** and **Save**.

The sender and the receiving mesh use the same skeleton, so no retargeting is needed.

#### Step 5: Place the Mannequin that Follows

1. Drag the mannequin's skeletal mesh (in the template, a mesh in `Content/Characters/Mannequins/Meshes`) from the **Content Browser** into the level, a few metres away from the player start.
2. With the new actor selected, in the **Details** panel under **Animation**, set **Animation Mode** to **Use Animation Blueprint** and **Anim Class** to `ABP_O3DMannequin`.

#### Step 6: Play

1. Click **Play**. Capture starts only in Play In Editor or a game, not in the editor viewport.
2. In the **Live Link** panel, the subject `Mannequin` appears under the source, and the source reports that it is receiving.
3. Move the character. The placed mannequin plays the same animation in place: its pose comes from the character over the Loopback transport.

**If nothing moves:**
- **Auto Create Transport** must be ticked on the sender. Without it, capture runs but nothing is sent, and the Output Log shows a warning when capture starts.
- Both ends must use **Loopback** and the same **Channel Name** (empty on both is fine).
- The **Live Link Subject Name** on the **Live Link Pose** node must match the sender's **Subject Name**.
- The [source status](#source-status) in the Live Link panel says what the receiver sees. The sender logs under `LogO3DSenderComponent`.

To stream between two machines, keep this setup and change the transport on both ends; see [Transport Modules](#transport-modules).

---

## Core Concepts

### Architecture Overview

```
┌─────────────────┐         ┌──────────────┐         ┌─────────────────┐
│  Sender Actor   │         │  Transport   │         │ LiveLink Source │
│  ┌───────────┐  │         │              │         │  (Receiver)     │
│  │ Skeletal  │  │         │  ┌────────┐  │         │                 │
│  │   Mesh    │  │ Capture │  │Network │  │ Receive │  ┌──────────┐   │
│  └─────┬─────┘  ├────────>│  │  or    │──┼────────>│  │ LiveLink │   │
│        │        │         │  │In-Proc │  │         │  │  Client  │   │
│  ┌─────▼─────┐  │         │  └────────┘  │         │  └──────────┘   │
│  │O3DSender  │  │         │              │         │                 │
│  │Component  │  │         │   Optional   │         │   ┌──────────┐  │
│  └───────────┘  │         │  ┌────────┐  │         │   │  Audio   │  │
│                 │  Audio  │  │ Audio  │  │  Audio  │   │  Output  │  │
│  ┌───────────┐  ├────────>│  │ Stream │──┼────────>│   └──────────┘  │
│  │   Audio   │  │         │  └────────┘  │         │                 │
│  │  Capture  │  │         │              │         │                 │
│  └───────────┘  │         └──────────────┘         └─────────────────┘
└─────────────────┘
```

### Subject-Based Streaming

Each sender broadcasts data for a **subject** - a named stream of animation data. Multiple subjects can share the same transport channel. Receivers subscribe to all subjects on a channel and expose them as LiveLink subjects.

### Transport Modules

The plugin provides these transports. WebRTC comes from the free Open3DBroadcastWebRTC add-on plugin. Every transport carries mocap, audio and control.

| Transport | Best for | Network | Mocap delivery | Available in |
|-----------|----------|---------|----------------|--------------|
| **Loopback** | Testing, sender and receiver in one process | None (in-process) | Reliable, ordered | Open3DBroadcast |
| **TCP** | One receiver on a LAN | TCP, sender listens | Reliable, ordered | Open3DBroadcast |
| **UDP** | Lowest latency on a LAN; broadcast to a subnet | UDP, receiver listens | Unreliable | Open3DBroadcast |
| **NNG** | Several receivers on a LAN (Pub/Sub) | TCP | Pub/Sub unreliable; Pair, Push/Pull reliable, ordered | Open3DBroadcast |
| **MoQ** (Experimental) | Streaming through a relay (draft-07) | QUIC, outbound to the relay | Unreliable | Open3DBroadcast |
| **WebRTC** | Internet, NAT traversal, LiveKit rooms | WebRTC through a LiveKit server | Reliable, ordered | Open3DBroadcastWebRTC add-on |

See [Transport Modules](#transport-modules) for setup, ports and firewalls.

### Message Format

A mocap frame travels as an 8-byte header (with a CRC-32) followed by a FlatBuffer (`SubjectList`). Audio and control travel in a 24-byte envelope that starts with the magic `O3DU` and names the kind (Audio or Control) and codec (PCM16, Opus or O3DControl). UDP adds a fragment header when a message is split, and TCP a length prefix. Everything is little-endian. The byte layout is in [docs/wire-format.md](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/wire-format.md).

---

## Sender Setup

### Adding the Sender Component

**In Blueprint:**
1. Select your actor
2. Add Component → **O3D Sender Component**

**In C++:**
```cpp
#include "O3DSenderComponent.h"

// In your actor class
UO3DSenderComponent* Sender = CreateDefaultSubobject<UO3DSenderComponent>(TEXT("O3DSender"));
```

### Basic Configuration

#### Required Settings

- **Subject Name**: Unique identifier for this animation stream (e.g., "MainCharacter", "Player1")
- **Transport Name**: Which transport to use: **Loopback**, **TCP**, **UDP**, **NNG**, **MoQ**, or **WebRTC** with the add-on. It is greyed out until **Auto Create Transport** is ticked (see [Transport Configuration](#transport-configuration))

#### Capture Settings

- **Capture Rate Hz**: Target frame rate (default: 60.0)
  - Higher = smoother but more bandwidth
  - Actual rate limited by tick rate
- **Auto Start Capture**: Start streaming automatically on BeginPlay
- **Target Mesh**: the skeletal mesh to capture, picked from the actor's components; it works on Blueprint defaults too. Empty: the actor's skeletal mesh that drives its own pose (a mesh following a leader pose component is skipped); the log names the one chosen when the actor has several. **Resolved Target Mesh** (advanced, read-only) shows the mesh in use, which Blueprint can also set at runtime.

#### Transport Configuration

Tick **Auto Create Transport** in the **Transport** group first. While it is off (the default), **Transport Name** is greyed out, a warning says the component captures but sends nothing, and nothing is sent.

The **Transport** group of the Details panel (and the LiveLink **Add Source** panel on the receiver side) shows one row per option the selected transport declares. How the rows behave:

- An empty row uses the project default for that option if one is set (see [Project-wide transport defaults](#project-wide-transport-defaults)), otherwise the transport's default; the row shows that value as grey hint text. Opening the panel never writes a default into the component or the source settings, so selecting an actor does not mark the level as changed.
- A value is written when you commit it: Enter, moving focus away, or releasing a number box after dragging. Dragging a number does not write on every step.
- Each committed value is one undo step (**Ctrl+Z**). Changing the transport is one undo step too; the options of the previous transport are cleared with it, and undo brings them back.
- Credential rows (for example the WebRTC access token) open empty, show where the current value comes from, and never store the value in the level, the Blueprint or a LiveLink preset. They are not undo steps.
- NNG shows **Mode** and **Role** as two rows. An empty role uses the usual role for the mode.

The panels are part of the `Open3DBroadcastEditor` module, which loads only in the editor.

Switching a sender's or a LiveLink source's transport keeps the options you set for the previous one: switching back brings them back (TCP's `port` and UDP's `port` are kept apart). Credentials are never kept this way; they stay in the credential store. A LiveLink preset saves only the options of the transport its source uses.

#### Project-wide transport defaults

**Project Settings > Plugins > Open3DBroadcast** holds default transport options for the whole project, so a server URL, a relay URL, a port or the WebRTC token endpoint is set once instead of in every sender component and LiveLink source. They are saved in the project's `Config/DefaultGame.ini` and ship with packaged builds.

- **Sender Defaults** apply to sender components and **Receiver Defaults** to LiveLink receiver sources. Each is a list of transports (the registered name, for example `udp`, `nng` or `webrtc`) with the options to default, using the same keys as the **Transport Options Reference** below (for example `port`, `host`, `webrtc.url` or `webrtc.tokenEndpointUrl`).
- A component's or source's own value wins. An option it leaves empty takes the project default, and an option with neither takes the transport's built-in default.
- Credentials (for example `webrtc.token` or `webrtc.tokenEndpointAuth`) are never taken from here: `DefaultGame.ini` is committed and shipped, so such an entry is ignored with a warning in the log. Use the credential store or the credential's environment variable.
- A new LiveLink source starts with no options of its own, so the project defaults apply to it. Creating a source no longer changes the defaults for the next one.

There are no options common to every transport, and no `role`, `uri` or `stream_id` option: the sender component is always the sending end, and each transport declares its own options. The [Transport Options Reference](#transport-options-reference) lists each transport's keys with the names the panel shows. [Transport Modules](#transport-modules) says what to set on each end.

### Curve Filtering

Reduce bandwidth by filtering animation curves:

- **Enable Curve Filtering**: Enable delta-based filtering
- **Curve Epsilon**: Ignore changes smaller than this value (default: 0.0001)
- **Curve Delta Threshold**: Only send if change exceeds threshold (default: 0.001)
- **Include Curve Patterns**: Wildcards for curves to include (e.g., `face_*`)
- **Exclude Curve Patterns**: Wildcards for curves to exclude (e.g., `*_unused`)
- **Clamp Morph Curves to Unit**: Clamp morph targets to [0, 1] range
- **Drop NaN and Infinity**: Sanitize curve values (recommended: enabled)

### Controlling Capture

**In Blueprint:**
- Call `Start Capture` to begin streaming. It does nothing in the editor outside Play In Editor.
- Call `Stop Capture` to stop streaming
- Use `Is Capturing` to check current state
- Use `Get Connection State` (Idle, Connecting, Connected, Reconnecting, Failed) and `Get Transport Stats` (frames, bytes, drops, errors, queue) to show the link's health
- Use `Set Transport Name`, `Set Transport Option`, `Get Transport Option` and `Clear Transport Options` to configure the transport at runtime, for example from a server URL typed into your UI. They apply the next time capture starts, so call `Stop Capture` and `Start Capture` after changing them. `Transport Name` is read-only in Blueprint; use `Set Transport Name`, which keeps each transport's options apart.

Nothing is sent unless **Auto Create Transport** is on (it is off by default). Start Capture logs a warning when it is off and no C++ code consumes the frames.

**In C++:**
```cpp
// Start capturing and streaming
Sender->StartCapture();

// Stop
Sender->StopCapture();

// Check status
bool bIsActive = Sender->IsCapturing();
```

### Events and Delegates

**Blueprint events** (all on the game thread). Select the Sender Component, then in the Details panel's **Events** section click **+** next to the event, or in the Event Graph use **Assign On ...** on the component:
- `On Connection State Changed (New State)`: the transport's connection state changed (Connecting, Connected, Reconnecting, Failed, Idle). A change that happens between two frames is still announced.
- `On Capture Started` / `On Capture Stopped`: a capture run began or ended (Stop Capture or end of play).
- `On Sender Error (Message)`: capture could not start, the transport could not start, or it failed. The message says why and never contains a credential. After a transport that could not start, the state stays Failed until the next Start Capture.

**C++ delegates** (not available in Blueprint, because they fire for every frame):
- `OnDescriptorReady (Subject, Descriptor)`: the skeleton descriptor was built or changed.
- `OnPoseFrameReady (Subject, Frame)`: fired on the game thread with each sampled frame, before it is serialized. With the asynchronous pipeline (the default, `o3d.Sender.AsyncPipeline 1`) the frame carries the raw curves (`CurveList`, `RawCurveValues`); curve filtering runs afterwards on a worker thread.
- `OnSerializedFrame (Subject, Bytes, CaptureTime)`: fired after serialization with the wire bytes. It fires on the sender's worker thread while `o3d.Sender.AsyncPipeline` is 1, so a listener must be thread-safe and must not touch UObjects; bind and unbind it only while capture is stopped. Kept for one release.

**C++ Example** (the frame delegates are native multicast delegates, so bind with `AddUObject`, not `AddDynamic`):
```cpp
Sender->OnPoseFrameReady.AddUObject(this, &AMyActor::OnPoseReady);

void AMyActor::OnPoseReady(const FString& Subject, const FO3DSPoseFrame& PoseFrame)
{
    UE_LOG(LogTemp, Log, TEXT("%s: frame %llu with %d bones"),
           *Subject, PoseFrame.FrameIndex, PoseFrame.BoneLocalTransforms.Num());
}
```

Bind the Blueprint events from C++ with `AddDynamic` and a `UFUNCTION` handler, for example `Sender->OnSenderError.AddDynamic(this, &AMyActor::HandleSenderError)` with `void HandleSenderError(const FString& Message)`.

---

## Receiver Setup

### Creating a LiveLink Source

The receiver is implemented as a **LiveLink Source** and configured through Unreal's LiveLink panel.

#### Step-by-Step Setup

1. **Open LiveLink Panel**
   - **Window → Live Link**

2. **Add Source**
   - Click **+ Source**
   - Select **Open3D Receiver Source**

3. **Configure Source**
   - **Transport Name**: Must match sender (e.g., `webrtc`)
   - **Enable Audio**: Check to enable audio playback
   - **Audio Stream Label**: Filter by label (optional, leave empty for all)
   - **Audio Codec**: Preferred decoder (`PCM16` or `Opus`)

4. **Add Transport Options**
   - Expand **Transport Options**
   - Add entries matching your transport (see examples below)

5. **Create Source**
   - Click **Create Source**. It is disabled, with the reason shown above it, while the selected transport has no receiver or the transport refuses an option.
   - The source appears in the LiveLink list.

#### Source Status

The LiveLink list shows the source's status:

| Status | Meaning |
|---|---|
| Waiting for data via *transport* | The transport started; no frame has arrived yet. |
| Receiving via *transport* | Frames are arriving. |
| Unreadable data via *transport* | Packets arrive, but none this receiver can read (not Open3DStream data, or damaged). The log says more. |
| Error: the sender on *transport* needs wire protocol *N*; update this receiver | The sender is a newer version that this receiver cannot read. |
| No data received (via *transport*) | No frame for 2 seconds. It returns to Receiving when frames resume. |
| Reconnecting via *transport* | The transport lost its connection and is retrying. |
| Error: *reason* | The transport could not start or failed, with the reason (for example, no receiver registered for the transport, or the server refused the connection). After a failed start, LiveLink shows the source as invalid. |
| Invalid options: *reason* | The transport refused an option. |
| Transport *name* unloaded | The transport's module shut down. |

Concealment settings in the source's LiveLink **Settings** panel apply as soon as you edit them; turning concealment off frees its per-subject state.

### Creating a Source at Runtime (Blueprint or C++)

A game, or a tool that sets up its connection in code, can add a receiver source without the LiveLink panel:

- **Create Open3DStream LiveLink Source** (Open3DBroadcast | Receiver) takes the transport name (for example `udp` or `webrtc`), a map of transport options with the keys in the [Transport Options Reference](#transport-options-reference), a Context Name and Enable Audio. It returns true and a **LiveLink Source Handle** when the source was added. Options you leave out take the [project defaults](#project-wide-transport-defaults).
- Use LiveLink's own nodes on the handle: **Remove Source** to remove it, **Get Source Status** for its status line ("Receiving via ...", or why it is not), and **Is Source Still Valid**.
- Set credentials (for example a WebRTC token) with **Set Transport Secret** before creating the source, not in the options map: a credential found in the map is moved to the credential store for this session, with a warning.
- The source saves into a LiveLink preset like one made in the LiveLink panel.

In C++, `UO3DReceiverBlueprintLibrary::CreateLiveLinkSource` does the same (module `Open3DReceiver`).

### LiveLink Source Configuration Examples

#### Loopback (Testing)
```
Transport Name: loopback
Transport Options:
  - role: receiver
  - channel: test_channel
```

#### Sockets (TCP)
```
Transport Name: sockets
Transport Options:
  - role: receiver
  - uri: 0.0.0.0:9000
  - protocol: tcp
```

#### WebRTC (LiveKit)

Needs the free Open3DBroadcastWebRTC add-on; its USER_GUIDE covers the source settings. See [WebRTC Transport (free add-on)](#webrtc-transport-free-add-on).

### Applying Animation to Characters

Once the LiveLink source is receiving data, subjects will appear automatically.

#### Method 1: Live Link Component

1. Add a **Live Link Component** to your target actor
2. Configure:
   - **Subject Representation**: Your skeleton asset
   - **LiveLink Subject Name**: Name from sender (e.g., `MyCharacter`)

#### Method 2: Animation Blueprint

1. Open your Animation Blueprint
2. Add a **Live Link Pose** node
3. Connect to your output pose
4. Set **Live Link Subject Name** to your subject
5. Configure retargeting as needed

#### Method 3: Control Rig

1. Create a Control Rig asset
2. Add **Live Link** input
3. Map to your skeleton
4. Apply Control Rig to your character

---

## Transport Modules

A transport moves frames, audio and control between a sender component and a LiveLink source. Each transport registers under a name, and that name is what **Transport Name** (sender) and **Transport** (LiveLink source) list:

| Name | Module | Plugin |
|------|--------|--------|
| **Loopback** | `Open3DTransportLoopback` | Open3DBroadcast |
| **TCP**, **UDP** | `Open3DTransportSockets` | Open3DBroadcast |
| **NNG** | `Open3DTransportNNG` | Open3DBroadcast |
| **MoQ** (Experimental) | `Open3DTransportMoQ` | Open3DBroadcast |
| **WebRTC** | `Open3DTransportWebRTC` | Open3DBroadcastWebRTC add-on |

The sender and the receiver must use the same transport, and their options must point at each other. Every transport carries mocap, audio and control. The option keys, their panel names and defaults are in the [Transport Options Reference](#transport-options-reference).

### Delivery Guarantees

| Transport | Mocap delivery |
|-----------|----------------|
| Loopback | Reliable and ordered |
| TCP | Reliable and ordered |
| UDP | Unreliable |
| NNG | Pub/Sub: unreliable. Pair and Push/Pull: reliable and ordered |
| MoQ | Unreliable, in both delivery modes |
| WebRTC | Reliable and ordered; unreliable with `webrtc.prefer_lossy` |

Residual coding needs a reliable, ordered transport. On any other transport the sender sends frames without it and says so in a warning.

### Ports and Firewalls

| Transport | What must be reachable | Default |
|-----------|------------------------|---------|
| Loopback | Nothing: it never leaves the process | n/a |
| TCP | Inbound TCP on the sender's machine; the receiver connects to it | port 17700 |
| UDP | Inbound UDP on the receiver's machine; the sender sends to it | port 17800 |
| NNG | Inbound TCP on the end that listens: the sender for Pub/Sub and Pair, the receiver for Push/Pull (with the default roles) | 6000 Pub/Sub, 7000 Pair, 8000 Push/Pull |
| MoQ | Outbound UDP from both machines to the relay's host and port (QUIC). Nothing listens locally | the port in **Relay URL** |
| WebRTC | Outbound to the LiveKit server URL (`wss://`, usually port 443, sometimes a custom port such as 7880), and HTTPS to the token endpoint if you use one | see the add-on's USER_GUIDE |

TCP, UDP and NNG use the one port shown: audio and control travel on the same socket as the frames.

### Loopback Transport

**Purpose:** in-process streaming, for testing and for setups where the sender and the receiver run in the same editor or game.

**Setup:** select **Loopback** on both ends. That is all: both ends use the channel `default` unless you set **Channel Name**. Give several streams in one process different channel names to keep them apart.

**Characteristics:**
- No network. The sender and the receiver must run in one process, for example the editor and its Play In Editor session.
- Reliable and ordered. While the channel is full (**Queue Capacity**, 64 frames by default), new frames are refused rather than old ones dropped.
- Carries audio and control.

**Use when:** testing a capture pipeline, checking a character's setup, or debugging without network effects. The [Quick Start](#quick-start) uses it.

### TCP and UDP Transports

**Purpose:** direct streaming between two machines on a local network, without a server.

**TCP setup:**
1. On the sender, select **TCP**. The sender listens on **Port** (17700) on every interface (**Bind Address** `0.0.0.0`).
2. On the receiver, select **TCP**, set **Remote Host** to the sender's IP address and **Port** to the sender's port.
3. Allow inbound TCP on that port on the sender's machine.

**UDP setup:**
1. On the receiver, select **UDP**. It listens on **Port** (17800) on every interface (**Bind Address** `0.0.0.0`).
2. On the sender, select **UDP**, set **Destination Host** to the receiver's IP address and **Port** to the receiver's port.
3. Allow inbound UDP on that port on the receiver's machine.

Both default to `127.0.0.1`, so a sender and a receiver on one machine connect without changes.

**Characteristics:**
- **TCP** is reliable and ordered. The sender listens and accepts one receiver at a time. The receiver reconnects on its own when the connection drops or goes quiet for **Connection Timeout (seconds)**.
- **UDP** is unreliable: a lost datagram is a lost frame. A message larger than **Max Datagram Bytes** (64000) is split into fragments of **MTU** size (1200 bytes) and reassembled by the receiver; losing one fragment loses the frame. UDP sends to one address, or to a broadcast address when **Enable UDP Broadcast** is on at the sender and **Accept Broadcast Packets** at the receiver. There is no multicast.
- Both carry audio and control on the same socket as the frames.
- No encryption and no authentication. Use them on networks you trust.

**Use when:** two machines on one LAN or studio network, with no NAT between them. Pick TCP for one receiver and every frame; pick UDP for the lowest latency, or broadcast to several receivers on one subnet.

### NNG Transport

**Purpose:** streaming over [NNG](https://nng.nanomsg.org/) sockets, with one sender feeding several receivers (Pub/Sub) or a reliable link to one receiver (Pair, Push/Pull).

**Setup:**
1. Pick matching modes: **Publisher** on the sender with **Subscriber** on the receiver, **Pair** with **Pair**, or **Push** with **Pull**.
2. Leave **Role** at its default. With default roles exactly one end listens: the sender for Pub/Sub and Pair, the receiver for Push/Pull.
3. On the end that dials, set **Host** to the listening machine's IP address. The listening end listens on `0.0.0.0`.
4. Leave **Port** empty on both ends to use the mode's default (6000, 7000 or 8000), or set the same port on both.
5. Allow inbound TCP on that port on the listening machine.

**Characteristics:**
- TCP only (`tcp://` addresses). There are no IPC or WebSocket addresses.
- Pub/Sub is rated unreliable. Pair and Push/Pull are reliable and ordered.
- A subscriber receives every message the publisher sends. There are no subscription topics; a topic left in an old configuration is ignored with a warning.
- Carries audio and control. No encryption and no authentication.

**Use when:** several receivers on a LAN need the same stream (Pub/Sub), or one receiver needs a reliable link that either end can open (Pair, Push/Pull).

### MoQ Transport (Experimental)

**Purpose:** streaming through a Media over QUIC relay, so the sender and the receivers only make outbound connections.

The MoQ transport is Experimental. It implements draft-ietf-moq-transport-07, the relay must speak draft-07, and its options and behaviour can change between releases.

**What you need:** a MoQ relay that speaks draft-07 and that both machines can reach over UDP (QUIC). The plugin does not include a relay.

**Setup:**
1. On the sender, select **MoQ** and set **Relay URL**, for example `https://relay.example.com:443`.
2. Set **Track Namespace (optional)** and **Track Name (optional)** on the sender, for example `mocap/stage1` and `performer1`.
3. On the receiver, select **MoQ** and set the same **Relay URL**, **Track Namespace (optional)** and **Track Name (optional)**.

Set the namespace and track name on both ends. Left empty, they do not match: the sender uses `mocap/<Subject Name>` and `<Subject Name>`, the receiver `mocap/default` and `primary`. Start the namespace with `mocap/`: the transport then puts audio on `audio/...` and control on `control/...` with the same rest of the namespace.

**Tracks:** a sender publishes three tracks under one track name: mocap in the namespace you set (`mocap/<session>`), audio in `audio/<session>`, and control in `control/<session>`. A receiver subscribes to the mocap track, and to the audio and control tracks when it uses them.

**Delivery Mode** (sender only):
- **Stream** (default): every frame is delivered, in order.
- **Datagram**: late frames are dropped instead of waited for.

Both are rated unreliable, so residual coding is not used over MoQ.

**Characteristics:**
- Connections are outbound only: no port to open on either machine.
- The transport reconnects on its own, with a growing delay between attempts (0.5 to 10 seconds). An attempt that has not connected after 15 seconds (`connect_timeout`) is abandoned and retried.
- Carries audio and control.

**Use when:** senders and receivers are on different networks and you run, or have access to, a draft-07 MoQ relay.

### WebRTC Transport (free add-on)

**Purpose:** Cloud-ready streaming through a LiveKit server, with NAT traversal and audio support.

WebRTC is **not included in Open3DBroadcast**. It is a separate, free add-on plugin, **Open3DBroadcastWebRTC**, which adds "WebRTC" to the sender and receiver transport pickers.

- **Download:** **[DOWNLOAD LINK PLACEHOLDER: support-site URL for Open3DBroadcastWebRTC, to be added before release]**
- **Install:** copy the `Open3DBroadcastWebRTC` folder into your project's `Plugins/` folder, next to (or in addition to) your Open3DBroadcast install, enable **Open3DBroadcast WebRTC** in **Edit → Plugins**, and restart the editor. Nothing inside the Open3DBroadcast folder changes, so updating Open3DBroadcast does not remove it.
- **Versions must match:** each add-on build works only with the Open3DBroadcast release it was built for. With another release it registers nothing and logs `WebRTC transport not registered: Open3DBroadcastWebRTC was built for Open3DBroadcast transport API version N, ...`. Download the add-on build for your Open3DBroadcast version.
- **Removing it:** disable the add-on (or delete its folder) and restart. Every other transport keeps working. Components and LiveLink sources set to WebRTC keep their settings and report that the transport is not registered.

The add-on's own [USER_GUIDE](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/ProjectSandbox/Plugins/Open3DBroadcastWebRTC/USER_GUIDE.md) (also in the add-on's folder) covers LiveKit setup, credentials and automatic token fetch, audio, and WebRTC troubleshooting.

**Characteristics:** reliable and ordered (unreliable with `webrtc.prefer_lossy`). Carries audio and control. Both ends connect out to the LiveKit server.

**Use When:**
- Remote collaboration over internet
- NAT traversal required
- Audio streaming needed
- Multiple participants in a room

---

## Audio Streaming

### Audio Capture (Sender)

The sender can capture audio from two sources:

1. **Game Audio (Mix Mode)**: Captures from audio submix
2. **Microphone (Input Mode)**: Captures from microphone input

#### Enabling Audio

In **O3D Sender Component**:
1. Check **Enable Audio**
2. Configure **Audio Capture Config**:
   - **Audio Capture Mode**: `Mix` or `Input`
   - **Audio Codec**: `PCM16` (uncompressed) or `Opus` (compressed)
   - **Sample Rate**: 48000 recommended
   - **Num Channels**: 1 (mono) or 2 (stereo)
   - **Bitrate Kbps**: 64 recommended for Opus

#### Audio Capture Modes

**Mix Mode (Game Audio):**
```
Audio Capture Mode: Mix
Submix to Tap: [leave empty for main submix]
Game Gain: 1.0        # Volume multiplier
```

Captures audio from the game's audio output. Useful for:
- Broadcasting game sound effects
- Sharing music/ambience
- Full game audio capture

**Input Mode (Microphone):**
```
Audio Capture Mode: Input
Audio Input Device: [select from dropdown]
Mic Gain: 1.0         # Volume multiplier
```

Captures from microphone input. Useful for:
- Voice chat
- Commentary
- Live narration

The device list in the dropdown is cached: the editor reads it once at startup, and each
capture start in Input mode reads it again before opening the device. After plugging in a
microphone while the editor is open, call **Refresh Audio Input Devices** (Blueprint) or run
the console command `o3d.Sender.Audio.RefreshDevices` (it also logs the list).

#### Audio Timestamps

Audio is stamped on the same clock as the pose frames (`FPlatformTime::Seconds()` on the
sender), so a receiver can line the two up. The submix and microphone clocks are mapped onto
it, and device clock drift is followed. If you feed audio yourself with
`UO3DSenderAudioCaptureComponent::PushFrames`, pass the timestamp on that clock too.

#### Audio Stream Label

- The audio stream label is the sender's subject name: the sanitized **Subject Name**, or
  the generated `World/Actor/Component` name when that is empty. It is the same name the
  pose frames carry, and it follows renames. A sender with no skeletal mesh and no
  Subject Name uses `o3ds:audio`.
- Receivers can filter by label

#### Audio Codec Selection

**PCM16:**
- Uncompressed 16-bit audio
- High quality, high bandwidth
- ~1.5 Mbps for stereo 48kHz
- Zero latency encoding
- Use for: LAN, testing

**Opus:**
- Compressed, high-quality codec
- Configurable bitrate (16-128 kbps)
- Low latency (~20ms)
- Excellent quality at 64 kbps
- Use for: Internet, WebRTC
- Needs a sample rate of 8, 12, 16, 24 or 48 kHz and 1 or 2 channels, and a build with
  Opus (Win64 with `opus.lib`). Otherwise the sender sends PCM16, labelled PCM16, and logs
  a warning.

### Audio Playback (Receiver)

Audio is played back using the **O3D Remote Audio Component**.

#### Adding Audio Component

1. Add **O3D Remote Audio Component** to an actor
2. Configure:
   - **Receive Mode**: `Mix`, `Subject` or `Any Stream`
   - **Stream Label Filter**: play only streams with this label (empty: any)
   - **Gain**: Output volume multiplier
   - **Attenuation Settings**: Spatial audio (optional)

#### Receive Modes

**Mix Mode** (the default):
- Plays streams labelled `o3ds:mix`: the label the receiver gives audio when the transport sets none and the receiver source has no stream ID
- Most transports label audio with the sender's subject or stream name, so use **Any Stream** or **Subject** for those

**Subject Mode:**
- Plays the audio of the LiveLink subject named in **LiveLink Subject Name**
- Can be positioned in 3D space
- Follows subject's position

**Any Stream Mode:**
- Plays any stream (narrow it with **Stream Label Filter**)

In every mode the component plays one stream at a time: the first that matches, until it has sent nothing for a second. Audio from other matching streams is dropped meanwhile, not mixed in. To play several streams, add one component per stream with a **Stream Label Filter** each, or separate the receiver sources with **Context Name**.

#### Audio Bus

Audio is routed through a centralized **Audio Bus** singleton:
- All receivers publish to the bus
- All audio components subscribe to the bus
- Enables flexible routing and mixing

### Audio Troubleshooting

**No audio output:**
1. Check **Enable Audio** on sender and receiver
2. Check that animation arrives over the same transport. Every transport carries audio
3. Check the component's **Receive Mode** and **Stream Label Filter** match the stream's label (Mix plays only `o3ds:mix`)
4. Verify codec compatibility
5. Check Windows audio mixer for Unreal Engine volume

**Audio dropouts/crackling:**
1. Increase Opus bitrate
2. Check network bandwidth
3. Reduce capture rate or resolution
4. Use PCM16 for testing (eliminates codec issues)

**High latency:**
1. Use Opus instead of PCM16 (for network streams)
2. Reduce audio buffer sizes (advanced)
3. Use lower sample rate (32000 instead of 48000)

---

## Control Channel

The control channel lets a sender trigger cues and change parameters on every client that receives its stream. Typical uses are VFX, lighting and audio cues, environment parameters such as fog density or time of day, and character parameters such as an emotion or a prop's visibility. Control travels on the same transport as the sender's mocap and audio. It is one-way: from the sender to its receivers. Design: [ADR 0011](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/adr/0011-control-channel.md).

Control costs nothing when you don't use it. A sender puts no control bytes on the wire until gameplay calls the control API.

### Values and Events

Control carries two kinds of message:

| | Value | Event |
|---|---|---|
| What it is | Keyed state, for example `env.fog_density` = 0.3 | A one-off cue, for example `vfx.muzzle_flash` |
| Identity | A key, plus an optional target subject | A name, plus an optional target subject |
| Payload | One typed value | An optional typed value |
| Rule | The last value set wins. Clearing removes the key | Delivered at most once |
| Loss and late joiners | The sender re-sends all its values as a snapshot every `ControlSnapshotIntervalSeconds` (1 s by default). A client that joins late, or loses a message, has the current values within about one interval | Each event is sent `ControlEventRedundancy` times (3 by default) on consecutive ticks, and receivers drop the duplicates. An event older than 2 s is dropped, so a late joiner never sees old cues |

The target subject is optional. Leave it empty to aim at the whole stream. Set it to a character's LiveLink subject name to aim at that character; receivers filter on it.

Keys, event names and target subjects are case-sensitive strings (`FString`). `env.Fog` and `env.fog` are different keys.

**Value types** (`EO3DControlValueType`): `None`, `Bool`, `Int` (64-bit), `Float` (double), `String`, `Name`, `Vector`, `Quat`, `Transform`, `Color` (`FLinearColor`) and `Bytes`. A value is an `FO3DControlValue`. In Blueprint, build one with the **Make Control Value (...)** nodes (Bool, Integer64, Float, String, Name, Vector, Rotator, Quat, Transform, Color, Bytes) and read one with **Control As Bool**, **Control As Int**, **Control As Float**, **Control As String**, **Control As Vector**, **Control As Rotator**, **Control As Quat**, **Control As Transform**, **Control As Color** and **Control As Bytes**. Each `As` node has a `Success` output. Rotations are stored as quaternions; the Rotator nodes convert.

### Sending from the Sender Component

The control functions are on **O3D Sender Component**, under **Open3DBroadcast | Sender | Control**:

| Function | What it does |
|---|---|
| `FireControlEvent(EventName, Payload, TargetSubject)` | Sends an event. Returns false and logs a warning when it cannot be sent: the transport is not running, the transport does not carry control, or the name or payload is invalid or too large |
| `SetControlValue(Key, Value, TargetSubject)` | Sets a value. Allowed before capture starts; the value goes out when the transport starts. Setting the same value again sends nothing. Returns false and logs a warning when refused |
| `ClearControlValue(Key, TargetSubject)` | Removes a value from every receiver |
| `ClearAllControlValues()` | Removes every value this sender set |
| `GetControlValue(Key, TargetSubject, OutValue)` | Returns the value this sender holds for a key |

`TargetSubject` defaults to empty in Blueprint and C++.

Properties, in the same category:

| Property | Default | Description |
|---|---|---|
| `bAllowControlOnly` | false | Start the transport with no skeletal mesh and audio off, for an actor that only sends control. See [Control-only senders](#control-only-senders) |
| `ControlSnapshotIntervalSeconds` | 1.0 | How often all values are re-sent (0.25 to 10 s) |
| `ControlEventRedundancy` | 3 | Copies of each event, on consecutive ticks (1 to 5) |
| `ControlMaxValueRateHz` | 30 | How often one value is re-sent at most while it keeps changing (1 to 120). The latest value always wins |

**Blueprint example.** On a sender actor, call **Fire Control Event** with *Event Name* `vfx.muzzle_flash`, *Payload* from **Make Control Value (Name)** with `Rifle`, and *Target Subject* set to the character's subject name. To drive fog from a timeline, call **Set Control Value** every update with *Key* `env.fog_density` and **Make Control Value (Float)**.

**C++ example:**

```cpp
#include "O3DSenderComponent.h"
#include "O3DControlTypes.h"

void AMyStageController::FireLightCue(int32 CueNumber)
{
    // Event: a lighting cue with an Int payload, aimed at the whole stream.
    Sender->FireControlEvent(TEXT("light.cue"), FO3DControlValue::MakeInt(CueNumber));
}

void AMyStageController::SetFog(float Density)
{
    // Value: an environment parameter. Calling this every tick is fine (see Coalescing).
    Sender->SetControlValue(TEXT("env.fog_density"), FO3DControlValue::MakeFloat(Density));
}

void AMyStageController::SetEmotion(const FString& Subject, const FString& Emotion)
{
    // Value aimed at one character.
    Sender->SetControlValue(TEXT("char.emotion"), FO3DControlValue::MakeName(Emotion), Subject);
}
```

The module that calls these needs `Open3DSender` and `Open3DShared` in its `Build.cs` dependencies.

**Coalescing.** Several `SetControlValue` calls for the same key and target in one tick collapse to the last value, and all changes from one tick go out together. A parameter driven every frame therefore costs one entry per tick, and at most `ControlMaxValueRateHz` entries per second. Receivers that want smooth motion interpolate on their side.

**Lifetime.** Control starts and stops with the sender's transport and ticks with the component. The values survive `StopCapture` and `StartCapture`: on start, the sender sends a snapshot at once, so remote lights and parameters don't blink when you edit the sender's Details panel (which restarts it). Events that were still waiting to be sent when the transport stopped are dropped.

Each sender component has its own control source id, a fresh GUID per instance that is never saved. A duplicated actor gets its own id, so two senders never merge their values on a receiver.

### Control-only Senders

A stage, lighting or environment controller has no skeletal mesh. Add an **O3D Sender Component** to it, set the transport as usual, and tick `bAllowControlOnly`. `StartCapture` then starts the transport even with no target mesh and audio off. Such a sender streams no mocap, so receivers deliver its control on arrival (see [Alignment with mocap](#alignment-with-mocap)).

### Enabling Control on a Client

Receiving control is **off by default**. Control triggers gameplay, and UDP and NNG carry no authentication, so each client project decides whether to accept it. There are three ways to turn it on. The most specific one that is set wins:

1. **Per receiver source:** **Control Accept** on the Open3D Receiver Source's settings (`UO3DReceiverSourceSettings::ControlAccept`): `Project Default` (the default), `Enabled` or `Disabled`. Use this when you run several receiver sources and want control on only some of them.
2. **Runtime override:** `UO3DControlLibrary::SetControlReceiveEnabled(bool)` from Blueprint or C++. It overrides the project setting for the whole process until `ClearControlReceiveOverride()`. `IsControlReceiveEnabled()` reports the current state for sources set to Project Default. Use it from a menu, a login flow or your own config, at any time after startup, with no restart.
3. **Project setting (the shipping default):** **Project Settings > Plugins > Open3DBroadcast Control > Accept Control**. It is saved to your project's `Config/DefaultGame.ini`, which is staged into packaged builds, so ticking it is all a project does to ship a client (including a Shipping build) with control on:

   ```ini
   [/Script/Open3DReceiver.O3DControlSettings]
   bAcceptControl=True
   ```

Precedence: the per-source setting (unless it is Project Default), then the runtime override (if set), then the project setting.

The decision is made for each message, on the game thread, so a change applies at once:
- While control is off, the receiver drops control messages before parsing them.
- Turning it off discards that source's control state silently. No **On Control Value Cleared** events fire.
- Turning it on rebuilds the values from the next snapshot, within one snapshot interval.

There is no console variable or command-line switch that turns control on.

**Project settings** (`UO3DControlSettings`, in `DefaultGame.ini`):

| Setting | Default | Description |
|---|---|---|
| `bAcceptControl` | false | Accept control on receiver sources set to Project Default |
| `ControlAllowlist` | empty | Key and event-name prefixes to accept, for example `env.` or `vfx.`. Empty accepts every name. Case-sensitive |
| `bAlignControlToMocap` | true | Hold events and value changes until the mocap they were sent with is shown |
| `MaxAlignmentHoldMs` | 500 | Longest a change is held for alignment (0 to 5000 ms). After that it is delivered late, never dropped |
| `MaxControlLiveBytesPerSecond` | 65536 | Byte budget per sender for live control messages (advanced) |
| `MaxControlSnapshotBytesPerSecond` | 524288 | Separate byte budget per sender for snapshots, so live traffic cannot starve them (advanced) |
| `MaxControlKeysPerSource` | 1024 | Most values (plus pending clears) kept per sender (1 to 1024, advanced) |

`bAcceptControl`, `bAlignControlToMocap` and `MaxAlignmentHoldMs` apply at once. The allowlist and the limits are read when the receiver source starts; restart the source after changing them.

The default byte budgets are twice what a sender produces at most, so a default receiver never rate-limits a well-behaved sender. Input over a budget, beyond the key cap, or outside the allowlist is dropped and counted.

**Control never sets properties by reflection and never runs console commands.** The plugin hands you typed values and events; your Blueprint or C++ decides what each key and event name means. If you want a key to drive a property, write that mapping yourself, against your own list of allowed keys.

### Receiving: O3D Remote Control Component

Add **O3D Remote Control Component** (`UO3DRemoteControlComponent`) to any actor that should react to control, set its filters and bind its events. It listens from `BeginPlay` to `EndPlay`.

**Filters** (all case-sensitive; empty accepts everything):

| Property | Description |
|---|---|
| `StreamIdFilter` | Only changes received on this stream (the receiver source's stream id) |
| `SourceNameFilter` | Only changes from this sender. The source name is the object name of the actor that owns the sender component |
| `TargetSubjectFilter` | Only changes aimed at this subject (a character's LiveLink subject name) |
| `bIncludeUntargeted` | With a target filter set, also accept changes aimed at the whole stream. Default true |
| `NamePrefixFilter` | Only keys and event names starting with this, for example `vfx.` |

**Events:**
- **On Control Event** (`EventName`, `Value`, `Meta`): a sender fired an event.
- **On Control Value Changed** (`Key`, `Value`, `Meta`): a sender set a value, or the value changed. A value that has not changed does not fire again when a snapshot repeats it.
- **On Control Value Cleared** (`Key`, `Meta`): a sender cleared a value, or the sender went away. A sender that sends nothing for 30 s is dropped, and its values are reported as cleared.

**Queries:**
- `GetControlValue(Key, TargetSubject, OutValue)`: the current value of a key, from the first sender that passes `SourceNameFilter`.
- `GetAllControlValues()`: every current value from senders that pass the filters, as `FO3DControlEntry` (`Key`, `TargetSubject`, `Value`, `SourceId`).

`FO3DControlMeta` tells you where a change came from: `SourceId`, `SourceName`, `StreamId`, `TargetSubject`, `SenderTimeSec` (sender clock), `Epoch`, `Version` (values) and `EventId` (events).

**Blueprint example.** On a light actor, add an O3D Remote Control Component with `NamePrefixFilter` = `light.`. Bind **On Control Event**, compare *Event Name* with `light.cue`, read the cue number with **Control As Int**, and play your cue.

### C++: FO3DControlBus

The component is built on `FO3DControlBus` (`O3DControlBus.h`, module `Open3DShared`), the control counterpart of the audio bus. Receiver sources publish to it, and you can listen to it directly. It is **game thread only**.

```cpp
#include "O3DControlBus.h"

void UMyWeatherSubsystem::Start()
{
    Handle = FO3DControlBus::OnChange().AddUObject(this, &UMyWeatherSubsystem::OnControl);
}

void UMyWeatherSubsystem::OnControl(const FO3DControlChange& Change)
{
    // Change is valid only during the call; copy what you keep.
    if (Change.Kind == FO3DControlChange::EKind::ValueChanged && Change.Name == TEXT("env.fog_density"))
    {
        bool bOk = false;
        const double Density = UO3DControlValueLibrary::ControlAsFloat(Change.Value, bOk);
        if (bOk)
        {
            ApplyFog(Density);
        }
    }
}

void UMyWeatherSubsystem::Stop()
{
    FO3DControlBus::OnChange().Remove(Handle);
}
```

`FO3DControlChange` has `Kind` (`ValueChanged`, `ValueCleared` or `Event`), `Name` (the key or event name), `Value` and `Meta`. To read the current table, use `FO3DControlBus::GetSources()`, `GetValues(SourceId)` and `FindValue(SourceId, Key, Target)`. `FO3DControlBus::SetReceiveOverride` and `GetReceiveOverride` back the runtime enable functions above.

Two receiver sources can hear the same sender (UDP multicast, one MoQ track, or a duplicated LiveLink source). The bus drops an event it has already published and ignores a value change that is not newer than the one it holds, so each change reaches listeners once.

### Alignment with Mocap

Each control message carries its sender time on the same clock as the sender's mocap frames. With `bAlignControlToMocap` on (the default), the receiver holds an event or value change until the pose LiveLink is showing for that sender's mocap stream has reached the change's sender time. A cue therefore plays with the motion it was fired against, not ahead of it by the receiver's buffering delay.

Alignment never stalls control:
- A change from a control-only sender, or from a sender whose mocap is not received here, is delivered at once.
- A change whose mocap stream has had no packet for 200 ms (the performer's stream paused) is delivered at once.
- A change held longer than `MaxAlignmentHoldMs` (500 ms by default) is delivered late, never dropped.
- Changes from one sender are delivered in the order they were sent.
- In LiveLink **Timecode** mode a change is held only while the sender's mocap carries its
  timecode and this engine has one (see [Timecode Mode](#timecode-mode)): the receiver then works
  out which sender time the shown pose has from LiveLink's read time (the engine timecode less
  *Timecode Frame Offset*, and less the clock offset when *Use Timecode Smooth Latest* is on).
  Otherwise nothing is held.

Turn `bAlignControlToMocap` off to deliver changes as they arrive. That suits environment changes that need no lip-sync accuracy and should not wait.

### Limits

| Limit | Value |
|---|---|
| Control envelope on the wire | 1,100 bytes including its header. Control is never fragmented on any transport. Snapshots are split into parts to fit |
| Key, event name, target subject | 128 bytes of UTF-8 each |
| `String` and `Name` values, `Bytes` values | 512 bytes each |
| Items (sets, clears, events) per message | 64 |
| Values per sender | 1,024 |
| Event time-to-live | 2 s |
| Sender output | at most 32 KiB/s live and 256 KiB/s for snapshots, per sender component |

A single value or event that cannot fit one control envelope is refused at the API: `SetControlValue` or `FireControlEvent` returns false and logs why. Doubles must be finite.

### Transport Support

| Transport | Control | Delivery |
|---|---|---|
| TCP | Yes | Reliable and ordered, in the same queue as frames. Refused while no receiver is connected |
| UDP | Yes | Unreliable. One datagram per control message, never fragmented. Refused if `udp.maxdatagram` is below the envelope size |
| NNG | Yes | Same socket as frames. Pair and push/pull are reliable; pub/sub is treated as unreliable |
| Loopback | Yes | Its own queue (up to 1,024 messages), independent of the frame and audio queues |
| MoQ | Yes | Its own `control/<session>` track with stream delivery, announced on every connect. Not ordered against mocap |
| WebRTC (add-on) | Yes | Reliable, ordered LiveKit data on the `__o3d.ctl` label. Refused while the sender is not connected (before Start, after Stop, while reconnecting); the publisher retries. Avoid a subject named `__o3d.ctl` |

See the Control row in [Transport_Module_Comparison.md](Transport_Module_Comparison.md#4-functional-parity-matrix) for details. Event redundancy is 3 on every transport today, including the reliable ones; the extra copies are a few dozen bytes each.

**Older receivers.** A receiver built before the control channel ignores control silently on TCP, UDP and NNG, and never subscribes to the control track on MoQ. Its mocap is unaffected. Update every receiver before relying on control.

### Control Troubleshooting

**Nothing arrives on the client:**
1. Check that control is enabled on the client: **Accept Control** in Project Settings, a `SetControlReceiveEnabled(true)` call, or **Control Accept** = `Enabled` on the receiver source. `IsControlReceiveEnabled()` tells you the process-wide state. A per-source `Disabled` beats everything else.
2. Check the allowlist. A key or event name that matches no prefix in `ControlAllowlist` is dropped. The match is case-sensitive.
3. Check the component's filters (`StreamIdFilter`, `SourceNameFilter`, `TargetSubjectFilter`, `NamePrefixFilter`). They are case-sensitive.
4. Check that the transport carries control. WebRTC does not yet.
5. On the sender, check the Output Log for `FireControlEvent(...) was not sent` or `SetControlValue(...) was refused`, with the reason.
6. For a sender with no mesh and no audio, tick `bAllowControlOnly`, or `StartCapture` does not start the transport.

**`FireControlEvent` returns false:** the transport is not running yet (events need a running transport; values do not), the transport does not carry control, or the name is empty, too long or the payload too large.

**Values arrive but events don't:** events are not stored. An event fired before the client enabled control, connected or joined is never delivered; an event older than 2 s is dropped. Use a value for anything a late joiner must see.

**Cues arrive late:** with alignment on, a cue waits for the matching pose, up to `MaxAlignmentHoldMs`. That delay is the pose's own buffering delay. Turn `bAlignControlToMocap` off for changes that should not wait.

**Values flicker or reset:** this should not happen on a sender restart. Check that two sender components are not setting the same key on one stream, and that nothing calls `ClearAllControlValues` unexpectedly.

**Logging:** the sender logs refused calls as warnings under `LogO3DSenderComponent`. The receiver logs rejected control messages at Verbose under `LogO3DReceiverSource`:

```
log LogO3DReceiverSource Verbose
```

---

## LiveLink Integration

### Understanding LiveLink

LiveLink is Unreal's system for receiving real-time animation data from external sources. Open3DBroadcast integrates as a LiveLink source, making it compatible with all LiveLink-enabled workflows.

### Subject Management

**Automatic Subject Creation:**
- Subjects appear automatically when sender starts
- Subject name matches sender's `Subject Name`
- A subject that stops receiving frames is cleared after the source's **Inactive Subject Timeout** (default 5 s, 0 = never): LiveLink shows it with no data, but keeps the subject and its settings (preprocessors, interpolation, translators), and its next frame makes it valid again

**Subject Data Includes:**
- Bone transforms (local space)
- Animation curves
- Morph target weights
- Frame timing information

### Retargeting Animation

#### Same Skeleton
If sender and receiver use the same skeleton:
1. LiveLink will apply animation directly
2. No retargeting needed

#### Different Skeletons
If skeletons differ:

**Option 1: IK Retargeting (UE5)**
1. Create an IK Rig for source skeleton
2. Create an IK Rig for target skeleton
3. Create an IK Retargeter asset
4. Map bones between skeletons
5. Use retargeter in Animation Blueprint

**Option 2: Control Rig**
1. Create Control Rig asset
2. Add LiveLink input
3. Map source bones to target rig
4. Drive target skeleton from rig

**Option 3: Animation Blueprint**
1. Add LiveLink Pose node
2. Use Modify Bone nodes to adjust
3. Custom retargeting logic

### LiveLink Presets

Save and load LiveLink configurations:

1. Configure your LiveLink sources
2. Click **Preset** dropdown in LiveLink panel
3. **Save Preset** with a name
4. Load preset later to restore configuration

Useful for:
- Switching between different streaming setups
- Team sharing of configurations
- Quick setup for common scenarios

### Timecode Mode

The receiver sets each LiveLink frame's **SceneTime**, so a LiveLink source in **Timecode** mode
selects frames by timecode, and Take Recorder and Sequencer can line received motion up with
other timecoded sources (cameras, audio, other machines):

- **The sender has a synchronized timecode provider** (Project Settings > Engine > General
  Settings > Timecode Provider: genlock, LTC, or `SystemTimeTimecodeProvider`): every frame
  carries the sender's timecode from when it was sampled, and the receiver uses it as the
  frame's SceneTime. Frames line up with other sources only when both machines' timecode
  sources agree (the same genlock or LTC, or system clocks kept in sync).
- **The sender has none:** the receiver derives a SceneTime from when it presents the frame, on
  its own engine timecode. Frames are ordered and evenly spaced, but this is the receiver's
  time, not the performance's.
- **Neither engine has a timecode:** SceneTime is left at its default, as before, and LiveLink
  warns that the engine has no timecode in Timecode mode. Use EngineTime mode instead.

A subject keeps the frame rate of its first timecode; later timecodes at another rate are
converted to it, because a rate change empties LiveLink's buffer for the subject. Concealed
frames continue the subject's timeline.

**Whole-frame timecode.** A provider that reports whole frames (`SystemTimeTimecodeProvider`
does by default: *Generate Full Frame*) gives every sender tick within one timecode frame the
same timecode, so a 60 Hz sender on 24 fps timecode stamps two or three frames alike. LiveLink
keeps them all, in arrival order, and warns once. For smooth playback either turn off
*Generate Full Frame* on the sender's provider, or turn on *Generate Sub Frame* in the LiveLink
source's buffer settings and set *Source Timecode Frame Rate* to the sender's capture rate.

### Multiple Subjects

You can stream multiple subjects simultaneously:

**Sender Side:**
- Add O3D Sender Component to multiple actors
- Give each a unique **Subject Name**
- All can share the same transport

**Receiver Side:**
- Single LiveLink source receives all subjects
- Each subject appears separately in LiveLink
- Apply to different characters as needed

**Example:**
```
Actor1 → O3DSender(Subject: "Character1") ┐
Actor2 → O3DSender(Subject: "Character2") ├→ WebRTC Transport → LiveLink Source
Actor3 → O3DSender(Subject: "Prop1")      ┘
                                              ↓
                                          LiveLink Subjects:
                                          - Character1
                                          - Character2
                                          - Prop1
```

### Separate Receivers: Runtime Contexts

By default every receiver source publishes its audio and control to the same place, and every
O3D Remote Audio and Remote Control component listens there. When two receivers carry the same
stream labels or control keys (a monitor receiver next to a live one, or several PIE clients),
give them **Context Names**:

- **Receiver source**: *Context Name* (advanced, in the source's connection settings).
- **O3D Remote Audio Component** and **O3D Remote Control Component**: *Context Name*.
- **O3D Sender Component**: *Context Name* (advanced). For a sender it selects only where its
  performance metrics are counted; what it sends is unchanged.

A component hears only receiver sources with the same Context Name; an empty name is the default
context, which is what every source and component uses unless you set one. Names are not case
sensitive. Each context also has its own performance metrics: `o3d.DumpMetrics` prints the
default context, then each named one, and `o3d.ResetMetrics` resets them all. The HUD and the
CSV export show the default context only.

Context Names are read when a source is created, when a remote component begins play and when a
sender starts its transport; changing one later takes effect the next time. They separate audio, control and metrics only: LiveLink
subject names are shared by the whole process, because Unreal Engine 5.7 has one LiveLink
client per process (PIE clients included), so keep subject names distinct across receivers.

---

## Configuration Reference

### Sender Component Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **SubjectName** | String | "" | Unique identifier for this stream |
| **ContextName** | Name | (empty) | Runtime context for this sender's metrics; see [Separate Receivers](#separate-receivers-runtime-contexts) |
| **CaptureRateHz** | Float | 60.0 | Capture rate in Hz, at most one capture per tick; 0 captures every tick |
| **bAutoStartCapture** | Bool | true | Start capturing on BeginPlay |
| **TargetMeshComponent** (Target Mesh) | Component picker | (empty) | Skeletal mesh to capture; empty picks the actor's skeletal mesh that drives its own pose |
| **TargetMesh** (Resolved Target Mesh) | Object | null | The mesh in use; read-only in the Details panel, settable from Blueprint at runtime |
| **TransportName** | Name | "loopback" | Transport module to use (read-only in Blueprint; use Set Transport Name) |
| **bAutoCreateTransport** | Bool | false | Create and run the selected transport. Off: nothing is sent unless C++ code consumes the frames |
| **TransportOptions** | Map | {} | Key-value transport configuration |

#### Curve Filtering Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **bEnableCurveFiltering** | Bool | false | Enable curve filtering |
| **CurveEpsilon** | Float | 0.0001 | Minimum significant change |
| **CurveDeltaThreshold** | Float | 0.001 | Change threshold for emission |
| **IncludeCurvePatterns** | Array | [] | Wildcard patterns to include |
| **ExcludeCurvePatterns** | Array | [] | Wildcard patterns to exclude |
| **bClampMorphCurvesToUnit** | Bool | true | Clamp morphs to [0,1] |
| **bDropNaNAndInfinity** | Bool | true | Sanitize invalid values |

#### Audio Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **bEnableAudio** | Bool | false | Enable audio streaming |
| **AudioCaptureMode** | Enum | Mix | Mix (game) or Input (mic) |
| **AudioInputDevice** | String | "" | Microphone device name |
| (audio stream label) | - | subject name | Not a property: the resolved subject name (see Audio Stream Label) |
| **AudioCodec** | Name | "PCM16" | Audio codec (PCM16/Opus) |

#### Audio Capture Config

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **SampleRate** | Int | 48000 | Audio sample rate (Hz) |
| **NumChannels** | Int | 1 | 1=mono, 2=stereo |
| **BitrateKbps** | Int | 64 | Opus bitrate (16-128) |
| **GameGain** | Float | 1.0 | Game audio volume multiplier |
| **MicGain** | Float | 1.0 | Microphone volume multiplier |
| **SubmixToTap** | Object | null | Custom submix (or null for main) |

### Receiver Source Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **TransportName** | Name | "loopback" | Transport module to use |
| **ContextName** | Name | (empty) | Runtime context for audio, control and metrics; see [Separate Receivers](#separate-receivers-runtime-contexts) |
| **bEnableAudio** | Bool | false | Enable audio playback |
| **AudioStreamLabel** | String | "" | Filter by label (empty = all) |
| **AudioCodec** | Name | "Opus" | Preferred audio decoder |
| **TransportOptions** | Map | {} | Key-value transport configuration |

### Remote Audio Component Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **ContextName** | Name | (empty) | Plays audio from receiver sources with this Context Name only |
| **ReceiveMode** | Enum | Mix | Mix (`o3ds:mix` only), Subject, or Any Stream |
| **StreamLabelFilter** | String | "" | Play only streams with this label, case-insensitive (empty = any) |
| **LiveLinkSubjectName** | Subject | (none) | Subject for Subject mode |
| **Gain** | Float | 1.0 | Output volume multiplier |
| **Target Latency (ms)** | Float | 60 | Audio held before playback starts, to absorb network jitter. Playback waits for this much again after the audio runs out; when more than this plus 60 ms builds up, the oldest audio is dropped back to it, so latency cannot grow |
| **Allow Spatialization** | Bool | false | Spatialize the sound in 3D |
| **Override Attenuation** | Bool | false | Use per-instance attenuation instead of Attenuation Settings |
| **AttenuationSettings** | Object | null | Attenuation asset, used when Override Attenuation is off |
| **Attach Parent** | Component | (unset) | Attach the component to this component. Unset: it stays where it is placed; one with no parent attaches to the actor's root |
| **Auto Activate** | Bool | true | Start playback when audio first arrives. Off: audio is queued and plays after **Play** |

Blueprint: **Play** starts playback (now if audio has arrived, otherwise when it does); **Stop** stops it until the next **Play**. The component creates its internal audio component at Begin Play and destroys it at End Play.

### Transport Options Reference

Each transport declares its options, separately for the sender and the receiver. The **Transport** group of the sender's Details panel and the LiveLink source panel show one row per declared option, labelled with the name in the **Shown as** column. Blueprint (**Set Transport Option**, **Create Open3DStream LiveLink Source**), C++ and the [project-wide defaults](#project-wide-transport-defaults) use the key.

- An option you leave empty takes the project default, then the default below.
- Keys are case-insensitive. A key the selected transport does not read has no effect.
- The keys listed under "Not shown in the panel" are read by the transport, but have no row. Set them with **Set Transport Option** or in the project-wide defaults.

There is no `role` option: the sender component is always the sending end and the LiveLink source the receiving end.

#### Loopback

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `channel` | **Channel Name** | Sender, receiver | `default` | In-process channel. A receiver hears the senders on the same channel. Leading and trailing spaces and letter case are ignored |
| `loopback.maxqueue` | **Queue Capacity** | Sender | 64 | Frames the channel buffers (1 to 4096). While it is full, new frames are refused |

Not shown in the panel: `loopback.maxaudioqueue` (sender; audio buffers the channel holds, default 32).

#### TCP

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `bind` | **Bind Address** | Sender | `0.0.0.0` | Local address the sender listens on. `0.0.0.0` listens on every interface. Must be an IP address, not a host name |
| `port` | **Port** | Sender, receiver | 17700 | TCP port the sender listens on and the receiver connects to (1 to 65535) |
| `host` | **Remote Host** | Receiver | `127.0.0.1` | Address of the sender |
| `tcp.timeout` | **Connection Timeout (seconds)** | Receiver | 5 | The receiver reconnects when nothing (frames or keepalives) arrives for this long (1 to 60) |

Not shown in the panel:

| Key | Side | Default | Description |
|-----|------|---------|-------------|
| `tcp.maxqueue` | Sender | 4194304 | Bytes of frames queued for the receiver; frames beyond it are dropped |
| `tcp.maxqueueage` | Sender | 1000 | Queued frames older than this many milliseconds are dropped; 0 turns it off |
| `tcp.stalltimeout` | Sender | 2000 | Milliseconds a frame may make no progress before the sender drops the receiver |
| `tcp.keepalive` | Sender | 1000 | Milliseconds of silence after which the sender sends a keepalive; 0 turns it off |
| `tcp.connecttimeout` | Receiver | 5 | Seconds a connect may take before it is retried |
| `tcp.maxframe` | Receiver | 4194304 | Largest frame accepted, in bytes |
| `tcp.backoff` | Receiver | 500 | First reconnect delay in milliseconds; it doubles after each failed attempt |
| `tcp.maxbackoff` | Receiver | 5000 | Longest reconnect delay in milliseconds |

#### UDP

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `host` | **Destination Host** | Sender | `127.0.0.1` | Address the datagrams are sent to: the receiver's address, or a broadcast address |
| `host` | **Bind Address** | Receiver | `0.0.0.0` | Local address the receiver listens on. `0.0.0.0` listens on every interface |
| `port` | **Port** | Sender, receiver | 17800 | UDP port (1 to 65535) |
| `udp.broadcast` | **Enable UDP Broadcast** | Sender | false | Allow sending to a broadcast address |
| `udp.broadcast` | **Accept Broadcast Packets** | Receiver | false | Receive datagrams sent to a broadcast address |
| `udp.mtu` | **MTU** | Sender | 1200 | Size of each fragment, header included, when a message is split (256 to 65507) |
| `udp.maxdatagram` | **Max Datagram Bytes** | Sender, receiver | 64000 | Sender: largest message sent as one datagram; a larger one is split into fragments. Receiver: largest datagram accepted. 512 to 65507; keep it at least as large as the MTU |

Not shown in the panel: `udp.maxframe` (receiver; largest frame reassembled from fragments, default 4194304 bytes).

TCP and UDP keep their options apart, so switching between them keeps each one's `port`.

#### NNG

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `host` | **Host** | Sender, receiver | `0.0.0.0` when this end listens, `127.0.0.1` when it dials | Address to listen on, or the address of the end to dial |
| `port` | **Port** | Sender, receiver | 6000 (Pub/Sub), 7000 (Pair), 8000 (Push/Pull) | TCP port (1 to 65535) |
| `nng.mode` | **Mode** | Sender | `pub` | `pub` (**Publisher**), `pair` (**Pair**) or `push` (**Push**) |
| `nng.mode` | **Mode** | Receiver | `sub` | `sub` (**Subscriber**), `pair` (**Pair**) or `pull` (**Pull**) |
| `nng.role` | **Role** | Sender, receiver | the usual role for the mode | `server` (**Listen (server)**) or `client` (**Dial (client)**). Shown only for Pair, Push and Pull |
| `nng.qmax` | **Queue Capacity (MiB)** | Sender | 4 MiB | Bytes queued for a slow receiver before frames are dropped (1 to 512 MiB in the panel; the key holds bytes) |

#### MoQ (Experimental)

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `relay_url` | **Relay URL** | Sender, receiver | none (required) | The relay, for example `https://relay.example.com:443`. The relay must speak draft-ietf-moq-transport-07 |
| `track_namespace` | **Track Namespace (optional)** | Sender, receiver | `mocap/<session>` | Namespace of the mocap track. The sender's session is its **Subject Name**; the receiver's is `default` |
| `track_name` | **Track Name (optional)** | Sender, receiver | sender: the **Subject Name**; receiver: `primary` | Name of the track |
| `delivery_mode` | **Delivery Mode** | Sender | `stream` | `stream` (**Stream**) delivers every frame in order. `datagram` (**Datagram**) drops late frames instead of waiting for them |
| `queue_bytes` | **Queue Capacity (MiB)** | Sender | 8 MiB | Bytes queued for the relay before frames are dropped (1 to 256 MiB in the panel; the key holds bytes) |

Not shown in the panel: `connect_timeout` (sender and receiver; seconds before an unfinished connection attempt is abandoned and retried, 1 to 120, default 15) and `moq.session` (sender and receiver; the `<session>` part of the default namespace).

#### WebRTC

Provided by the Open3DBroadcastWebRTC add-on; its USER_GUIDE lists the `webrtc.*` options. See [WebRTC Transport (free add-on)](#webrtc-transport-free-add-on).

---

## Performance Tuning

### Optimizing Bandwidth

**Reduce Capture Rate:**
```
Capture Rate Hz: 30    # Instead of 60
```
- 50% bandwidth reduction
- Still smooth for most animations

**Enable Curve Filtering:**
```
Enable Curve Filtering: ✓
Curve Delta Threshold: 0.01
Exclude Curve Patterns: ["*_unused", "*_debug"]
```
- Reduces curve data significantly
- Only sends meaningful changes

**Use Opus for Audio:**
```
Audio Codec: Opus
Bitrate Kbps: 32      # Lower for voice-only
```
- 64 kbps stereo vs 1.5 Mbps PCM16
- Excellent quality at low bandwidth

**Simplify Skeleton:**
- Remove unused bones before streaming
- Reduce LOD for distant characters
- Filter unnecessary curves

### Optimizing Latency

**Increase Capture Rate:**
```
Capture Rate Hz: 90    # Higher refresh
```
- More responsive animation
- Increases bandwidth

**Use UDP for Sockets:**
```
Transport Options:
  - protocol: udp
```
- Lower latency than TCP
- May drop frames under packet loss

**Optimize Network:**
- Use wired connections
- Minimize network hops
- QoS prioritization for animation traffic

**WebRTC Tuning (Open3DBroadcastWebRTC add-on):**
```
webrtc.max_bitrate: 5000000    # 5 Mbps cap
webrtc.min_bitrate: 500000     # 500 Kbps floor
```

### Memory Optimization

**Limit Queue Sizes:**
```
loopback.queue: 32     # Smaller queue
```
- Reduces memory footprint
- May drop frames if consumer is slow

**Subject Cleanup:**
- Subjects idle for longer than **Inactive Subject Timeout** (default 5 s) are cleared, not removed, so their LiveLink settings survive a pause
- The receiver forgets the stream state of senders idle that long

### CPU Optimization

**Reduce Serialization Cost:**
- Lower capture rate
- Fewer curves
- Simpler skeletons

**Multi-Threading:**
- Sender serialization is async-safe
- Transport I/O typically on separate threads
- Audio encoding threaded (Opus)

### Monitoring Performance

**Transport Statistics:**

Access via C++:
```cpp
FO3DTransportStats Stats = Sender->GetTransportStats();
UE_LOG(LogTemp, Log, TEXT("Sent %lld frames, %lld bytes, avg latency %.2f ms"),
       Stats.FramesSent, Stats.BytesSent, Stats.AverageLatencyMs);
```

**Available Metrics:**
- `FramesSent` / `FramesReceived`
- `BytesSent` / `BytesReceived`
- `DroppedFrames`
- `AverageLatencyMs` / `MaxLatencyMs`

**LiveLink Status:**
- Green: Receiving recent data
- Yellow: Stale data (1-5 seconds old)
- Red: No data (>5 seconds)

---

## Troubleshooting

### Common Issues

#### "Subject not appearing in LiveLink"

**Symptoms:** Sender is capturing but receiver shows no subjects

**Solutions:**
1. **Check transport configuration:**
   - Verify `Transport Name` matches on both sides
   - Verify transport options match (especially `channel` or `stream_id`)
   - Check `role` is set correctly (sender vs receiver)

2. **Check network connectivity:**
   - For Sockets: Verify IP and port accessibility
   - For WebRTC: Verify server URL and token validity
   - Test with Loopback first to isolate network issues

3. **Check LiveLink source status:**
   - Open LiveLink panel
   - Look for error messages
   - Try removing and re-creating source

4. **Enable debug logging:**
   ```
   Console: "log LogO3DReceiver Verbose"
   ```

#### "Audio not working"

**Symptoms:** Animation works but no audio output

**Solutions:**
1. **Transport:** every transport carries audio (Loopback, TCP, UDP, NNG, MoQ and WebRTC). Check that the sender and the LiveLink source use the same transport and matching options, and that animation arrives.

2. **Check audio settings:**
   - Sender: `Enable Audio` checked
   - Receiver: `Enable Audio` checked
   - Codec matches or receiver supports sender's codec

3. **Check audio component:**
   - Add `O3D Remote Audio Component` to scene
   - Verify `Receive Mode` and `Stream Label`
   - Check `Gain` is not zero

4. **Check audio device:**
   - Windows Sound Settings → Unreal Engine not muted
   - For Input mode: Microphone permissions granted
   - For Mix mode: Game actually producing audio

#### "High latency / lag"

**Symptoms:** Animation is delayed or choppy

**Solutions:**
1. **Optimize capture rate:**
   - Try lower capture rate (30 Hz) first
   - Increase only if smooth enough

2. **Network optimization:**
   - Use wired connection
   - Close bandwidth-heavy applications
   - For WebRTC: Check TURN server performance

3. **Codec selection:**
   - For Audio: Use Opus instead of PCM16
   - Lower Opus bitrate if needed

4. **Transport selection:**
   - LAN: Use Sockets (lowest latency)
   - Internet: WebRTC required, latency expected

#### "Frames dropping / choppy animation"

**Symptoms:** Animation stutters or skips frames

**Solutions:**
1. **Check CPU usage:**
   - High CPU can cause dropped ticks
   - Reduce capture rate
   - Simplify scene

2. **Network bandwidth:**
   - Too much data for connection
   - Enable curve filtering
   - Lower capture rate
   - Use Opus for audio

3. **Queue overflow:**
   - Receiver not processing fast enough
   - Increase queue size (Loopback only)
   - Reduce data rate

4. **Check transport stats:**
   ```cpp
   UE_LOG(LogTemp, Warning, TEXT("Dropped %lld frames"), Stats.DroppedFrames);
   ```

#### "WebRTC is not in the transport list" or "WebRTC connection fails"

1. **Is the add-on installed and enabled?** WebRTC comes from the free Open3DBroadcastWebRTC add-on ([WebRTC Transport (free add-on)](#webrtc-transport-free-add-on)). Without it, "WebRTC" is not in the transport pickers.
2. **Does the add-on match your Open3DBroadcast release?** Search the Output Log for `WebRTC transport not registered`. The message says whether the transport API version differs (install the matching add-on build) or `livekit_ffi.dll` could not be loaded.
3. **Connection problems** (server URL, tokens, TURN): see the troubleshooting section of the add-on's USER_GUIDE.

### Debug Logging

Enable verbose logging for troubleshooting:

**Console Commands:**
```
log LogO3DSender Verbose
log LogO3DReceiver Verbose
log LogO3DTransportSockets Verbose
log LogO3DWebRTCSender Verbose
log LogO3DWebRTCReceiver Verbose
```

The two `LogO3DWebRTC*` categories exist only when the Open3DBroadcastWebRTC add-on is installed.

**In DefaultEngine.ini:**
```ini
[Core.Log]
LogO3DSender=Verbose
LogO3DReceiver=Verbose
LogO3DWebRTCSender=Verbose
LogO3DWebRTCReceiver=Verbose
```

---

## Advanced Topics

### Custom Transport Implementation

You can implement custom transports by extending the transport interfaces.

**Required Interfaces:**
- `IOpen3DSender`: For sending data
- `IOpen3DReceiver`: For receiving data

**Registration:**
```cpp
// In your transport module's Startup
FO3DSenderRegistry::Get().RegisterFactory(
    FName("mycustom"),
    []() -> IOpen3DSender* { return new FMyCustomSender(); }
);

FO3DReceiverRegistry::Get().RegisterFactory(
    FName("mycustom"),
    []() -> IOpen3DReceiver* { return new FMyCustomReceiver(); }
);
```

**See:** Existing transport implementations in `Source/Open3DTransport*/` for examples.

### Multi-Subject Workflows

**Scenario:** Multiple characters in a scene, each with different update rates

```cpp
// High-priority character: 60 Hz
CharacterA->Sender->CaptureRateHz = 60.0f;
CharacterA->Sender->SubjectName = "Hero";

// Background character: 30 Hz
CharacterB->Sender->CaptureRateHz = 30.0f;
CharacterB->Sender->SubjectName = "NPC1";

// Prop: 15 Hz
Prop->Sender->CaptureRateHz = 15.0f;
Prop->Sender->SubjectName = "MovingProp";
```

All share the same transport, each identified by subject name.

### Runtime Transport Switching

**Scenario:** Start with loopback for testing, switch to WebRTC for production

```cpp
// Stop current capture
Sender->StopCapture();

// Change transport (SetTransportName keeps loopback's options for when you switch back)
Sender->SetTransportName(FName("webrtc"));
Sender->SetTransportOption(TEXT("webrtc.url"), TEXT("wss://production.server.com"));
// A credential option goes to the credential store for this session, never into the level
Sender->SetTransportOption(TEXT("webrtc.token"), ProductionToken);

// Restart
Sender->StartCapture();
```

### Custom Audio Processing

**Scenario:** Process audio before sending

1. Subclass `UO3DSenderAudioCaptureComponent`
2. Override `ProcessAudioBuffer` to apply effects
3. Use custom component instead of standard one

**Example:**
```cpp
void UMyAudioCapture::ProcessAudioBuffer(float* Buffer, int32 NumFrames)
{
    // Apply noise gate
    for (int32 i = 0; i < NumFrames * NumChannels; ++i)
    {
        if (FMath::Abs(Buffer[i]) < NoiseThreshold)
            Buffer[i] = 0.0f;
    }

    // Call base implementation
    Super::ProcessAudioBuffer(Buffer, NumFrames);
}
```

### Bandwidth Estimation

**Uncompressed (no filtering):**
```
Skeleton: 100 bones × 7 floats (Transform) × 4 bytes = 2.8 KB
Curves: 50 curves × 4 bytes = 0.2 KB
Frame size: ~3 KB

At 60 Hz: 3 KB × 60 = 180 KB/s = 1.44 Mbps
```

**With curve filtering (50% reduction):**
```
Frame size: ~2.9 KB
At 60 Hz: ~1.39 Mbps
```

**Audio (Opus 64 kbps stereo):**
```
Audio: 64 kbps

Total: 1.39 + 0.064 = 1.45 Mbps
```

**Audio (PCM16 48kHz stereo):**
```
Audio: 48000 × 2 × 2 bytes × 8 = 1.536 Mbps

Total: 1.39 + 1.536 = 2.93 Mbps
```

### Production Checklist

Before deploying to production:

- [ ] Test with loopback transport first
- [ ] Verify all subjects appear in LiveLink
- [ ] Test audio on the transport you deploy with
- [ ] Measure bandwidth usage
- [ ] Test network disconnection handling
- [ ] Verify retargeting works on target characters
- [ ] Configure appropriate capture rates
- [ ] Enable curve filtering if needed
- [ ] Set up monitoring/logging
- [ ] Document transport configuration for team
- [ ] Test firewall/NAT scenarios (for WebRTC)
- [ ] Prepare fallback configurations

---

## Additional Resources

### Documentation

- **Plugin README**: [README.md](README.md) - Installation and build info
- **Transport Comparison**: [Transport_Module_Comparison.md](Transport_Module_Comparison.md) - Detailed transport comparison
- **WebRTC Guide**: [Open3DBroadcastWebRTC USER_GUIDE](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/ProjectSandbox/Plugins/Open3DBroadcastWebRTC/USER_GUIDE.md) - setup of the free WebRTC add-on

### Support

- **GitHub Issues**: Report bugs and feature requests
- **Documentation**: This guide and module-specific docs
- **Code Examples**: See `Source/*/Private/` for implementation examples

### Next Steps

1. **Start simple**: Use Loopback transport for learning
2. **Experiment**: Try different transports and settings
3. **Optimize**: Tune for your specific use case
4. **Scale**: Move to WebRTC for remote/multi-user
5. **Customize**: Implement custom transports if needed

---

**Happy Streaming!**
