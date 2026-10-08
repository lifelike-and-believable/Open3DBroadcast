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
11. [Blueprint API Reference](#blueprint-api-reference)
12. [Performance Tuning](#performance-tuning)
13. [Troubleshooting](#troubleshooting)
14. [Known Limitations](#known-limitations)
15. [Privacy](#privacy)
16. [Updating and Removing the Plugin](#updating-and-removing-the-plugin)
17. [Advanced Topics](#advanced-topics)
18. [FAQ](#faq)
19. [Additional Resources](#additional-resources)

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
- **Several transports**: Loopback (in-process), TCP, UDP, NNG and MoQ (Experimental); WebRTC with the free Open3DBroadcastWebRTC add-on
- **LiveLink integration**: received subjects drive animation through LiveLink
- **Curve and morph target support** for facial animation
- **Control channel** for cues and parameters sent with the animation
- **Status and statistics**: connection state, transport counters and error events, also in Blueprint

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

### Transports at a Glance

The plugin provides these transports. WebRTC comes from the free Open3DBroadcastWebRTC add-on plugin. Every transport carries mocap, audio and control.

| Transport | Best for | Network | Mocap delivery | Available in |
|-----------|----------|---------|----------------|--------------|
| **Loopback** | Testing, sender and receiver in one process | None (in-process) | Reliable, ordered | Open3DBroadcast |
| **TCP** | One receiver on a LAN | TCP, sender listens | Reliable, ordered | Open3DBroadcast |
| **UDP** | Lowest latency on a LAN; several receivers by multicast or broadcast | UDP, receiver listens | Unreliable | Open3DBroadcast |
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
2. In the **Components** panel, click **Add** and choose **O3D Sender** (search for "O3D Sender")

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

There are no options common to every transport, and no `role`, `uri` or `stream_id` option: the sender component is always the sending end, and each transport declares its own options (NNG's `nng.role` only chooses whether an end listens or dials). The [Transport Options Reference](#transport-options-reference) lists each transport's keys with the names the panel shows. [Transport Modules](#transport-modules) says what to set on each end.

### Curve Filtering

Reduce bandwidth by filtering animation curves:

- **Enable Curve Filtering**: Enable delta-based filtering
- **Curve Epsilon**: Ignore changes smaller than this value (default: 0.0005)
- **Curve Delta Threshold**: Only send if change exceeds threshold (default: 0.001)
- **Include Curve Patterns**: Wildcards for curves to include (e.g., `face_*`)
- **Exclude Curve Patterns**: Wildcards for curves to exclude (e.g., `*_unused`)
- **Clamp Morph Curves to Unit**: Clamp morph targets to [0, 1] range
- **Drop NaN and Infinity**: Sanitize curve values (recommended: enabled)

Curve Epsilon and Curve Delta Threshold do not apply while residual coding or quantization is on (see [Encoding Properties](#encoding-properties)).

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

1. Open **Window → Virtual Production → Live Link**.
2. Click **Add Source** and choose **Open3DStream Receiver**.
3. In the panel that opens:
   - **Transport**: the same transport as the sender, for example **UDP**.
   - **Enable Audio**: tick it to play the audio the stream carries (see [Audio Playback](#audio-playback-receiver)).
   - **Audio Codec**: available while **Enable Audio** is ticked. **Transport Default** (the default), **PCM16** or **Opus** (Opus only in builds with Opus). Leave it at **Transport Default** unless you have a reason.
   - **Context Name** (advanced): see [Separate Receivers](#separate-receivers-runtime-contexts).
   - Below these, one row per option the transport declares, for example **Remote Host** and **Port** for TCP. Empty rows use the project default or the transport's default, shown as grey hint text. The rows behave as on the sender; see [Transport Configuration](#transport-configuration).
4. Click **Create Source**. It is disabled, with the reason shown above it, while the selected transport has no receiver or the transport refuses an option.

The source appears in the Live Link panel and shows its [status](#source-status). Its subjects appear under it as frames arrive. The settings available after creation, in the source's **Settings** panel, are listed in [Receiver Source Settings](#receiver-source-settings).

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

What to set in the source panel for each transport. The sender's side is in [Transport Modules](#transport-modules).

| Transport | Set on the receiver | Leave at the default |
|-----------|---------------------|----------------------|
| **Loopback** | Nothing, or the sender's **Channel Name** | Everything |
| **TCP** | **Remote Host**: the sender's IP address. **Port**: the sender's port if it is not 17700 | **Connection Timeout (seconds)** |
| **UDP** | **Port**: the port the sender sends to if it is not 17800. **Multicast Group** if the sender sends to one. **Bind Address** `0.0.0.0` when the sender is on another machine | **Bind Address** (`127.0.0.1`, this machine only) |
| **NNG** | **Mode**: **Subscriber**, **Pair** or **Pull**, matching the sender. **Host**: the sender's IP address when this end dials | **Role**, **Port** |
| **MoQ** | **Relay URL**, **Track Namespace (optional)** and **Track Name (optional)**, the same as on the sender | |
| **WebRTC** | See the Open3DBroadcastWebRTC add-on's USER_GUIDE | |

### Applying Animation to Characters

Once the source receives data, its subjects appear in the Live Link panel. To drive a skeletal mesh with one, use an Animation Blueprint, as in the [Quick Start](#step-4-make-an-animation-blueprint-that-reads-the-subject):

1. Create or open an Animation Blueprint for the mesh's skeleton.
2. In the **AnimGraph**, add a **Live Link Pose** node and connect it to **Output Pose**.
3. Set **Live Link Subject Name** on the node to the sender's **Subject Name**.
4. Set the mesh's **Anim Class** to this Animation Blueprint.

When the sender's and the receiver's skeletons differ, see [Retargeting Animation](#retargeting-animation).

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
| WebRTC | Outbound to the LiveKit server URL (`wss://`, usually port 443, sometimes a custom port such as 7880), the ports your LiveKit server uses for WebRTC media (see its configuration), and HTTPS to the token endpoint if you use one | see the add-on's USER_GUIDE |

TCP, UDP and NNG use the one port shown: audio and control travel on the same socket as the frames.

### Network Exposure

TCP, UDP and NNG carry no authentication and no encryption: anyone who can reach a listening port can receive the stream (TCP, NNG) or inject frames into it (UDP, NNG). So the end that listens accepts only this machine by default:

| Transport | The end that listens | Default | To accept other machines |
|---|---|---|---|
| TCP | The sender | **Bind Address** `127.0.0.1` | Set the sender's **Bind Address** to `0.0.0.0`, or to the address of one network interface |
| UDP | The receiver | **Bind Address** `127.0.0.1` | Set the receiver's **Bind Address** to `0.0.0.0`, or to one interface's address |
| NNG | The sender for Pub/Sub and Pair, the receiver for Push/Pull (default roles) | **Host** `127.0.0.1` | Set the listening end's **Host** to `0.0.0.0`, or to one interface's address |

A listener bound to any other address logs a warning when it starts: "... reachable from other machines. The stream has no authentication or encryption ...". To change the default for a whole project, set the option once in **Project Settings > Open3DBroadcast** for that transport and role.

On a shared network, prefer one interface's address over `0.0.0.0`, let the firewall admit only the machines that need the stream, and use MoQ or WebRTC, which run over encrypted connections, across untrusted networks.

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
1. On the sender, select **TCP** and set **Bind Address** to `0.0.0.0` (it listens on **Port** 17700). The default, `127.0.0.1`, accepts only this machine; see [Network Exposure](#network-exposure).
2. On the receiver, select **TCP**, set **Remote Host** to the sender's IP address and **Port** to the sender's port.
3. Allow inbound TCP on that port on the sender's machine.

**UDP setup:**
1. On the receiver, select **UDP** and set **Bind Address** to `0.0.0.0` (it listens on **Port** 17800). The default, `127.0.0.1`, accepts only this machine.
2. On the sender, select **UDP**, set **Destination Host** to the receiver's IP address and **Port** to the receiver's port.
3. Allow inbound UDP on that port on the receiver's machine.

**UDP multicast setup** (one sender, any number of receivers, on networks whose switches pass multicast):
1. Pick a group in `239.0.0.0/8` (addresses for your own site), for example `239.255.79.51`.
2. On each receiver, select **UDP**, set **Multicast Group** to the group and **Bind Address** to `0.0.0.0`. To run several receivers on one machine, turn on **Share Port** on each of them.
3. On the sender, select **UDP** and set **Destination Host** to the group. **Multicast TTL** (1) keeps the datagrams on the local network; raise it only to cross routers that forward multicast. **Multicast Loopback** (on) lets receivers on the sender's machine hear it.

On a receiver, **Allowed Senders** (a comma-separated list of IP addresses) drops datagrams from every other address. A UDP receiver otherwise accepts frames from anyone who can reach its port, so set it whenever the receiver listens beyond `127.0.0.1`. It filters by source address, which an attacker on the same network can forge; it keeps out stray senders, not a determined one.

Every address defaults to `127.0.0.1`, so a sender and a receiver on one machine connect without changes, and nothing is reachable from other machines until you set it.

**Characteristics:**
- **TCP** is reliable and ordered. The sender listens and accepts one receiver at a time. The receiver reconnects on its own when the connection drops or goes quiet for **Connection Timeout (seconds)**.
- **UDP** is unreliable: a lost datagram is a lost frame. A message larger than the sender's **MTU** (1200 bytes, header included) is split into fragments of that size and reassembled by the receiver; losing one fragment loses the frame. Control messages are always sent whole. UDP sends to one address, to a multicast group that any number of receivers join, or to a broadcast address when **Enable UDP Broadcast** is on at the sender.
- Both carry audio and control on the same socket as the frames.
- No encryption and no authentication. Use them on networks you trust.

**Use when:** two machines on one LAN or studio network, with no NAT between them. Pick TCP for one receiver and every frame; pick UDP for the lowest latency, or multicast to several receivers.

### NNG Transport

**Purpose:** streaming over [NNG](https://nng.nanomsg.org/) sockets, with one sender feeding several receivers (Pub/Sub) or a reliable link to one receiver (Pair, Push/Pull).

**Setup:**
1. Pick matching modes: **Publisher** on the sender with **Subscriber** on the receiver, **Pair** with **Pair**, or **Push** with **Pull**.
2. Leave **Role** at its default. With default roles exactly one end listens: the sender for Pub/Sub and Pair, the receiver for Push/Pull.
3. On the end that listens, set **Host** to `0.0.0.0` (the default, `127.0.0.1`, accepts only this machine). On the end that dials, set **Host** to the listening machine's IP address.
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

Left empty on both ends, the namespace is `mocap/default` and the track `primary`, so one sender and one receiver on a relay connect without them. Set them, the same on both ends, to run several streams on one relay. Start the namespace with `mocap/`: the transport then puts audio on `audio/...` and control on `control/...` with the same rest of the namespace. Any other namespace is used as it is for mocap, and audio and control get `audio/` and `control/` in front of it.

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

In the **O3D Sender** component's **Audio** group:
1. Tick **Enable Audio** (off by default).
2. Set:
   - **Audio Capture Mode**: **Mix (Main Submix or Custom)** for game audio, or **Input (Microphone)**.
   - **Audio Codec**: **PCM16** (uncompressed, the default) or **Opus** (compressed).
   - **Sample Rate**: 48000 (the default).
   - **Num Channels**: 1 (mono, the default) or 2 (stereo). PCM16 takes up to 8; Opus 1 or 2.
   - **Bitrate Kbps**: the Opus bitrate, 64 by default; 0 lets the encoder choose. PCM16 ignores it.

#### Audio Capture Modes

**Mix Mode (Game Audio):**
```
Audio Capture Mode: Mix (Main Submix or Custom)
Submix to Tap: [leave empty for main submix]
Game Gain: 1.0        # Volume multiplier
```

Captures audio from the game's audio output. Useful for:
- Broadcasting game sound effects
- Sharing music/ambience
- Full game audio capture

**Input Mode (Microphone):**
```
Audio Capture Mode: Input (Microphone)
Audio Input Device: [select from dropdown; empty uses the default device]
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
- On the receiver, the O3D Remote Audio Component picks streams by label with **Receive Mode** and **Stream Label Filter**; see [Receive Modes](#receive-modes).

#### Audio Codec Selection

**PCM16:**
- Uncompressed 16-bit audio
- About 1.5 Mbit/s for stereo at 48 kHz (48000 × 2 channels × 16 bits)
- Use for: LAN, testing

**Opus:**
- Compressed
- **Bitrate Kbps** sets the bitrate (64 by default; 0 lets the encoder choose)
- Use for: limited bandwidth, the internet
- Needs a sample rate of 8, 12, 16, 24 or 48 kHz and 1 or 2 channels, and a build with
  Opus (Win64 with `opus.lib`). Otherwise the sender sends PCM16, labelled PCM16, and logs
  a warning.

### Audio Playback (Receiver)

Audio is played back using the **O3D Remote Audio Component**.

#### Adding Audio Component

1. Add **O3D Remote Audio Component** to an actor
2. Configure:
   - **Receive Mode**: **Mix (o3ds:mix)**, **Subject (LiveLink)** or **Any Stream**
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
1. Raise **Target Latency (ms)** on the O3D Remote Audio Component (60 by default), so it absorbs more network jitter
2. Check network bandwidth; on a limited link use Opus
3. Increase the Opus bitrate
4. Use PCM16 for testing (eliminates codec issues)

**High latency:**
1. Lower **Target Latency (ms)** on the O3D Remote Audio Component. Too low a value causes dropouts on a jittery network
2. On a limited link, use Opus instead of PCM16

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

**Value types** (`EO3DControlValueType`): `None`, `Bool`, `Int` (64-bit), `Float` (double), `String`, `Name`, `Vector`, `Quat`, `Transform`, `Color` (`FLinearColor`) and `Bytes`. A value is an `FO3DControlValue`. In Blueprint, build one with the **Make Control Value (...)** nodes (Bool, Integer64, Float, String, Name, Vector, Rotator, Quat, Transform, Color, Bytes) and read one with **Control as Bool**, **Control as Int**, **Control as Float**, **Control as String**, **Control as Vector**, **Control as Rotator**, **Control as Quat**, **Control as Transform**, **Control as Color** and **Control as Bytes**. Each `as` node has a `Success` output. Rotations are stored as quaternions; the Rotator nodes convert.

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

1. **Per receiver source:** **Control Accept** in the Open3DStream Receiver source's **Settings** panel (`UO3DReceiverSourceSettings::ControlAccept`): `Project Default` (the default), `Enabled` or `Disabled`. Use this when you run several receiver sources and want control on only some of them.
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

**Blueprint example.** On a light actor, add an O3D Remote Control Component with `NamePrefixFilter` = `light.`. Bind **On Control Event**, compare *Event Name* with `light.cue`, read the cue number with **Control as Int**, and play your cue.

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

Two receiver sources can hear the same sender (UDP multicast or broadcast, one MoQ track, or a duplicated LiveLink source). The bus drops an event it has already published and ignores a value change that is not newer than the one it holds, so each change reaches listeners once.

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
4. Check that the sender's transport is running and the receiver source is connected to it (see its [status](#source-status)). Control travels on the same transport as the mocap, and every transport carries it (see [Transport Support](#transport-support)).
5. On the sender, check the Output Log for `FireControlEvent(...) was not sent` or `SetControlValue(...) was refused`, with the reason.
6. For a sender with no mesh and no audio, tick `bAllowControlOnly`, or `StartCapture` does not start the transport.

**`FireControlEvent` returns false:** the transport is not running yet (events need a running transport; values do not), or the name is empty, too long or the payload too large.

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
- A new subject gets the project's LiveLink defaults for the Animation role (**Project Settings > Live Link > Default Role Settings**), as subjects LiveLink creates itself do: by default the Animation interpolation processor, so LiveLink blends between frames instead of showing the closest one. Change it per subject in the LiveLink window
- A subject that stops receiving frames is cleared after the source's **Inactive Subject Timeout Seconds** (default 5 s, 0 = never): LiveLink shows it with no data, but keeps the subject and its settings (preprocessors, interpolation, translators), and its next frame makes it valid again

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
- Add an O3D Sender component to each actor. Each component sends one subject and runs its own transport.
- Give each a unique **Subject Name**.

**Receiver Side:**
- **Loopback** and **UDP**: one LiveLink source receives every sender on its channel, or every sender that sends to its address and port.
- **TCP**, **NNG** and **MoQ**: a source connects to one sender (one host and port, or one track). Give each sender its own **Port** (TCP, NNG) or **Track Name** (MoQ), and add one source per sender.
- Each subject appears separately in LiveLink. Apply each to a different character.

**Example (UDP, three senders in one game, one receiver):**
```
Actor1 → O3D Sender (Subject: "Character1") ┐
Actor2 → O3D Sender (Subject: "Character2") ├→ UDP to 192.168.1.20:17800 → LiveLink Source
Actor3 → O3D Sender (Subject: "Prop1")      ┘
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
| **TransportOptions** | Map | {} | The selected transport's options. Not shown as a property: the **Transport** group shows one row per option, and Blueprint uses **Set Transport Option**. Keys are in the [Transport Options Reference](#transport-options-reference) |

The control properties (`bAllowControlOnly`, `ControlSnapshotIntervalSeconds`, `ControlEventRedundancy`, `ControlMaxValueRateHz`) are in [Sending from the Sender Component](#sending-from-the-sender-component).

#### Curve Filtering Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **bEnableCurveFiltering** | Bool | false | Enable curve filtering |
| **CurveEpsilon** | Float | 0.0005 | Ignore curve changes smaller than this |
| **CurveDeltaThreshold** | Float | 0.001 | Send a new value only when it changes by more than this |
| **IncludeCurvePatterns** | Array | [] | Wildcard patterns to include |
| **ExcludeCurvePatterns** | Array | [] | Wildcard patterns to exclude |
| **bLogFilteredCurves** | Bool | false | Log the curves the filter drops (verbose) |
| **bClampMorphCurvesToUnit** | Bool | true | Clamp morphs to [0,1] |
| **bDropNaNAndInfinity** | Bool | true | Send NaN and infinite curve values as 0 |

#### Encoding Properties

By default every frame carries the full pose. Residual coding and quantization make frames smaller; both are **off by default**, and the plugin never turns either on for you. Both need receivers that read wire protocol 2, as this release does; older receivers drop such frames.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **bEnableResidualCoding** | Bool | false | Send the difference from a predicted pose instead of the full pose. Used only on a reliable, ordered transport (see [Delivery Guarantees](#delivery-guarantees)); on any other the sender logs one warning and sends without it. Wins over quantization when both are on |
| **ResidualPredictor** | Enum | Linear | **Hold (reduces to legacy last-sent delta)**, **Linear (recommended default)** or **Quadratic** |
| **ResidualKeyframeIntervalFrames** | Int | 300 | Send a residual keyframe (absolute values) every this many frames; 0: only when needed |
| **ResidualDeltaThreshold** | Float | 0.0001 | A channel whose residual is smaller than this is left out of the frame |
| **bEnableQuantization** | Bool | false | Send changed channels with 8 or 16 bits instead of 32 where they fit. Works on lossy transports too: a lost frame only delays a change until the next one or the next full sync |
| **QuantizationByteRange** | Float | 0.01 | Largest change from the last full sync sent with 8 bits |
| **QuantizationHalfRange** | Float | 1.0 | Largest change sent with 16 bits; larger ones use full precision. Never below Quantization Byte Range |
| **QuantizationDeltaThreshold** | Float | 0.0001 | A channel that changed less than this is left out of the frame |
| **FullSyncIntervalSeconds** | Float | 1.0 | With residual coding or quantization on, send the full skeleton and pose at least this often (0.25 to 10 s), so a receiver that joins late or lost a frame recovers. Ignored when both are off |

While residual coding or quantization is on, Curve Epsilon and Curve Delta Threshold do not apply.

#### Audio Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **bEnableAudio** | Bool | false | Capture and send audio. Off: no audio is captured |
| **AudioCaptureMode** | Enum | Mix | **Mix (Main Submix or Custom)**: game audio. **Input (Microphone)**: a microphone |
| **AudioInputDevice** | Name | None | Microphone, picked from the device list. None, or a name not in the list, uses the default device |
| (audio stream label) | - | subject name | Not a property: the resolved subject name (see Audio Stream Label) |
| **AudioCodec** | Name | PCM16 | **PCM16** or **Opus** |

#### Audio Capture Config

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| **SampleRate** | Int | 48000 | Sample rate sent (8000 to 48000 Hz). Receivers play 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100 or 48000; Opus takes 8000, 12000, 16000, 24000 or 48000. Another rate is sent at the nearest of those, with a warning |
| **NumChannels** | Int | 1 | 1 or 2 with Opus, up to 8 with PCM16 |
| **BitrateKbps** | Int | 64 | Opus bitrate in kbit/s (0 or more; the slider goes to 256). 0 lets the encoder choose. PCM16 ignores it |
| **GameGain** | Float | 1.0 | Gain on captured game audio; 1 leaves it unchanged |
| **MicGain** | Float | 1.0 | Gain on captured microphone audio; 1 leaves it unchanged |
| **SubmixToTap** | Object | null | Submix captured in Mix mode; empty: the main submix |

### Receiver Source Properties

Set in the **Open3DStream Receiver** panel when you create the source (or as the arguments of **Create Open3DStream LiveLink Source**). They apply when the source is created.

| Panel name | Property | Default | Description |
|------------|----------|---------|-------------|
| **Transport** | `TransportName` | **Loopback** | Transport to receive with |
| One row per option | `TransportOptions` | empty | The transport's options; see the [Transport Options Reference](#transport-options-reference). Not edited as a map |
| **Enable Audio** | `bEnableAudio` | false | Play the audio the stream carries |
| **Audio Codec** | `AudioCodec` | **Transport Default** (None) | Codec to decode with: **Transport Default**, **PCM16** or **Opus**. Available while **Enable Audio** is ticked |
| **Context Name** (advanced) | `ContextName` | empty | Runtime context for audio, control and metrics; see [Separate Receivers](#separate-receivers-runtime-contexts) |

There is no audio stream label on the source: the receiver labels each audio stream itself, and the [O3D Remote Audio Component](#remote-audio-component-properties) chooses which stream it plays.

### Receiver Source Settings

After the source is created, select it in the Live Link panel. Its **Settings** panel shows LiveLink's own settings and these. They apply as soon as you edit them.

| Setting | Default | Description |
|---------|---------|-------------|
| **Inactive Subject Timeout Seconds** | 5 | Seconds without a frame after which a subject's frames are cleared, so LiveLink shows it with no data. The subject and its LiveLink settings are kept; its next frame makes it valid again. 0: never |
| **Enable Concealment** | true | When frames stop arriving, predict or hold the pose instead of leaving the gap to LiveLink's interpolation |
| **Starvation Threshold Ms** | 50 | Gap since the last real frame, in ms, after which concealment starts |
| **Max Horizon Ms** | 150 | After this many ms of concealment with no real frame, stop predicting and hold the last pose |
| **Correction Window Ms** | 100 | When frames resume, blend from the concealed pose to the real one over this many ms instead of snapping. 0: snap |
| **Render Ahead Ms** | 0 | Predict this many ms beyond the newest real frame even without a gap, to hide latency at the cost of accuracy. 0: off |
| **Control Accept** | **Project Default** | **Project Default**, **Enabled** or **Disabled**. See [Enabling Control on a Client](#enabling-control-on-a-client) |

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

There is no `role` option that makes an end a sender or a receiver: the sender component is always the sending end and the LiveLink source the receiving end. NNG's `nng.role` only chooses whether that end listens or dials.

#### Loopback

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `channel` | **Channel Name** | Sender, receiver | `default` | In-process channel. A receiver hears the senders on the same channel. Leading and trailing spaces and letter case are ignored |
| `loopback.maxqueue` | **Queue Capacity** | Sender | 64 | Frames the channel buffers (1 to 4096). While it is full, new frames are refused |

Not shown in the panel: `loopback.maxaudioqueue` (sender; audio buffers the channel holds, default 32).

#### TCP

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `bind` | **Bind Address** | Sender | `127.0.0.1` | Local address the sender listens on. `127.0.0.1` accepts only this machine; `0.0.0.0` listens on every interface ([Network Exposure](#network-exposure)). Must be an IP address, not a host name |
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
| `host` | **Destination Host** | Sender | `127.0.0.1` | Address the datagrams are sent to: the receiver's address, a multicast group, or a broadcast address |
| `host` | **Bind Address** | Receiver | `127.0.0.1` | Local address the receiver listens on. `127.0.0.1` accepts only this machine; `0.0.0.0` listens on every interface |
| `port` | **Port** | Sender, receiver | 17800 | UDP port (1 to 65535) |
| `udp.broadcast` | **Enable UDP Broadcast** | Sender | false | Allow sending to a broadcast address |
| `udp.multicastttl` | **Multicast TTL** | Sender | 1 | When **Destination Host** is a multicast group: how many routers the datagrams may cross (1 to 255). 1 keeps them on the local network |
| `udp.multicastloop` | **Multicast Loopback** | Sender | true | When **Destination Host** is a multicast group: receivers on the sender's machine get the datagrams too |
| `udp.multicast` | **Multicast Group** | Receiver | empty | IPv4 multicast group to join (224.0.0.0 to 239.255.255.255). Empty joins none |
| `udp.allowsource` | **Allowed Senders** | Receiver | empty | Comma-separated IP addresses; datagrams from any other address are dropped and counted as receive errors. Empty accepts every sender |
| `udp.reuseaddr` | **Share Port** | Receiver | false | Let other receivers on this machine bind the same port, for several multicast receivers on one machine. Off, a second receiver on the port fails to start |
| `udp.mtu` | **MTU** | Sender | 1200 | Largest datagram sent, header included; a larger frame or audio packet is split into fragments of this size (280 to 65507) |
| `udp.maxdatagram` | **Max Datagram Bytes** | Receiver | 64000 | Largest datagram accepted (512 to 65507); keep it at least as large as the sender's MTU and its control messages. The sender still reads the key as the ceiling for control messages, but its panel no longer shows it |

Not shown in the panel: `udp.maxframe` (receiver; largest frame reassembled from fragments, default 4194304 bytes).

TCP and UDP keep their options apart, so switching between them keeps each one's `port`.

#### NNG

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `host` | **Host** | Sender, receiver | `127.0.0.1` | Address to listen on (`0.0.0.0` for every interface), or the address of the end to dial |
| `port` | **Port** | Sender, receiver | 6000 (Pub/Sub), 7000 (Pair), 8000 (Push/Pull) | TCP port (1 to 65535) |
| `nng.mode` | **Mode** | Sender | `pub` | `pub` (**Publisher**), `pair` (**Pair**) or `push` (**Push**) |
| `nng.mode` | **Mode** | Receiver | `sub` | `sub` (**Subscriber**), `pair` (**Pair**) or `pull` (**Pull**) |
| `nng.role` | **Role** | Sender, receiver | the usual role for the mode | `server` (**Listen (server)**) or `client` (**Dial (client)**). Shown only for Pair, Push and Pull |
| `nng.qmax` | **Queue Capacity (MiB)** | Sender | 4 MiB | Bytes queued for a slow receiver before frames are dropped (1 to 512 MiB in the panel; the key holds bytes) |

#### MoQ (Experimental)

| Key | Shown as | Side | Default | Description |
|-----|----------|------|---------|-------------|
| `relay_url` | **Relay URL** | Sender, receiver | none (required) | The relay, for example `https://relay.example.com:443`. The relay must speak draft-ietf-moq-transport-07 |
| `track_namespace` | **Track Namespace (optional)** | Sender, receiver | `mocap/default` | Namespace of the mocap track |
| `track_name` | **Track Name (optional)** | Sender, receiver | `primary` | Name of the track |
| `delivery_mode` | **Delivery Mode** | Sender | `stream` | `stream` (**Stream**) delivers every frame in order. `datagram` (**Datagram**) drops late frames instead of waiting for them |
| `queue_bytes` | **Queue Capacity (MiB)** | Sender | 8 MiB | Bytes queued for the relay before frames are dropped (1 to 256 MiB in the panel; the key holds bytes) |

Not shown in the panel: `connect_timeout` (sender and receiver; seconds before an unfinished connection attempt is abandoned and retried, 1 to 120, default 15) and `moq.session` (sender and receiver; the `<session>` part of the default namespace).

#### WebRTC

Provided by the Open3DBroadcastWebRTC add-on; its USER_GUIDE lists the `webrtc.*` options. See [WebRTC Transport (free add-on)](#webrtc-transport-free-add-on).

---

## Blueprint API Reference

The nodes and events the plugin adds, by class, with the names the Blueprint editor shows. Call them on the game thread; the events fire there. Most properties in the [Configuration Reference](#configuration-reference) can also be read and set from Blueprint (Get and Set nodes). On the sender, **Transport Name** and **Context Name** are read-only in Blueprint (use **Set Transport Name**), and **Target Mesh** is not exposed (set **Resolved Target Mesh** instead).

### O3D Sender Component

`UO3DSenderComponent` (module `Open3DSender`). Category **Open3DBroadcast | Sender**.

| Node | What it does | Notes |
|------|--------------|-------|
| **Start Capture** | Starts capturing and, with **Auto Create Transport**, starts the transport | Safe to call while capturing. Does nothing in the editor outside Play In Editor. Fires **On Capture Started**, or **On Sender Error** when capture cannot start |
| **Stop Capture** | Stops capturing and the transport | Fires **On Capture Stopped** |
| **Is Capturing** | True while capturing | Pure |
| **Get Last Start Capture Error** | Why the last **Start Capture** did not start capture; empty if it did | Pure |
| **Get Connection State** | The transport's [connection state](#connection-state-and-transport-stats) | Pure. Idle when no transport runs; Failed after a transport that could not start, until the next **Start Capture** |
| **Get Transport Stats** | The running transport's [counters](#connection-state-and-transport-stats) | Pure. Zero, with the current state, when no transport runs |
| **Get Transport Name** | The selected transport | Pure |
| **Set Transport Name** | Selects the transport, for example `UDP` | None selects Loopback. The previous transport's options are put away and come back when you select it again. Applies the next time capture starts |
| **Get Transport Option** | Reads one option of the selected transport by key | Pure. Always empty for a credential |
| **Set Transport Option** | Sets one option by key; an empty value removes it | A credential key goes to the credential store for this session, never into the level. Applies the next time capture starts |
| **Clear Transport Options** | Removes every option of the selected transport | Applies the next time capture starts |
| **Fire Control Event** | Sends a control event | See [Sending from the Sender Component](#sending-from-the-sender-component) |
| **Set Control Value** | Sets a control value | As above |
| **Clear Control Value** | Removes a control value | As above |
| **Clear All Control Values** | Removes every value this sender set | As above |
| **Get Control Value** | The value this sender holds for a key | Pure |
| **Get Available Audio Input Device Options** | The cached list of microphone names | Never enumerates the devices; see **Refresh Audio Input Devices** |
| **Refresh Audio Input Devices** | Enumerates the audio capture devices again | Static. Call it after plugging in a microphone. **Start Capture** in Input mode does it too |

Events (bind them in the Details panel's **Events** section, or with **Assign** nodes in the Event Graph):

| Event | Fires when | Parameters |
|-------|------------|------------|
| **On Connection State Changed** | The transport's connection state changed | New State |
| **On Capture Started** | **Start Capture** succeeded | |
| **On Capture Stopped** | Capture stopped (**Stop Capture**, or end of play) | |
| **On Sender Error** | Capture could not start, the transport could not start, or it failed | Message (never contains a credential) |

### O3D Sender Audio Capture Component

`UO3DSenderAudioCaptureComponent`. The sender component creates and drives one itself when **Enable Audio** is on.

| Node | What it does | Notes |
|------|--------------|-------|
| **Get Available Input Device Options** | The cached list of microphone names | Same list as the sender's **Get Available Audio Input Device Options** |

### Receiver: Create Open3DStream LiveLink Source

`UO3DReceiverBlueprintLibrary` (module `Open3DReceiver`). Category **Open3DBroadcast | Receiver**.

| Node | What it does | Notes |
|------|--------------|-------|
| **Create Open3DStream LiveLink Source** | Creates a receiver source and adds it to LiveLink, in a game or in the editor | Inputs: Transport Name, Options (a map with the keys of the [Transport Options Reference](#transport-options-reference)), Context Name, Enable Audio. Outputs: a LiveLink Source Handle and a Return Value that is false when the transport has no receiver or LiveLink is not available. See [Creating a Source at Runtime](#creating-a-source-at-runtime-blueprint-or-c) |

### Credentials

`UO3DCredentialLibrary` (module `Open3DShared`). Category **Open3DBroadcast | Credentials**. Write-only: there is no node that reads a credential back.

| Node | What it does | Notes |
|------|--------------|-------|
| **Set Transport Secret** | Stores a credential for this session | Inputs: Transport Name, Profile (empty: `default`), Key (a credential key the transport declares, for example `webrtc.token`), Value. Never written to disk. An empty Value clears it |
| **Clear Transport Secret** | Clears a credential, the session value and any copy remembered on this machine | An environment variable for the key still applies |

### O3D Remote Audio Component

`UO3DRemoteAudioComponent` (module `Open3DReceiver`). Category **Open3DBroadcast | Audio**.

| Node | What it does | Notes |
|------|--------------|-------|
| **Play** | Starts playing received audio, now if some has arrived, otherwise when it does | Needed when **Auto Activate** is off |
| **Stop** | Stops playback | Audio that arrives afterwards is not played until **Play** |

### Control

| Class | Nodes | Where they are described |
|-------|-------|--------------------------|
| `UO3DControlLibrary` | **Set Control Receive Enabled**, **Clear Control Receive Override**, **Is Control Receive Enabled** | [Enabling Control on a Client](#enabling-control-on-a-client) |
| `UO3DControlValueLibrary` | **Make Control Value (...)**, **Control as ...**, **Control Value to String** (a readable form of a value, for logs) | [Values and Events](#values-and-events) |
| `UO3DRemoteControlComponent` | **Get Control Value**, **Get All Control Values**; events **On Control Event**, **On Control Value Changed**, **On Control Value Cleared** | [Receiving: O3D Remote Control Component](#receiving-o3d-remote-control-component) |

### Connection State and Transport Stats

**EO3DBroadcastConnectionState** (returned by **Get Connection State**, passed by **On Connection State Changed**):

| Value | Meaning |
|-------|---------|
| **Idle** | Not started, or stopped |
| **Connecting** | Started and waiting for a first peer, session or connection |
| **Connected** | Able to deliver |
| **Reconnecting** | Was connected, lost it, and is retrying |
| **Failed** | The transport could not start or gave up. Stop and start capture again to retry |

**FO3DBroadcastTransportStats** (returned by **Get Transport Stats**; break the struct to read the fields):

| Field | Meaning |
|-------|---------|
| **State** | The connection state, as above |
| **Frames Sent**, **Bytes Sent** | Frames and bytes the transport sent |
| **Frames Received**, **Bytes Received** | Frames and bytes received (receiving transports) |
| **Dropped Frames** | Frames dropped because the transport could not take them |
| **Send Errors**, **Receive Errors** | Errors the transport counted |
| **Pending Frames** | Items waiting in the transport's send queue |
| **Average Latency Ms**, **Max Latency Ms** | Latency the transport measured, if it measures any |

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
Enable Curve Filtering: ticked
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

**Use UDP instead of TCP:**
- Select **UDP** on both ends (see [TCP and UDP Transports](#tcp-and-udp-transports))
- A lost datagram is a lost frame, but a frame is never held back behind a lost one

**Optimize Network:**
- Use wired connections
- Minimize network hops
- QoS prioritization for animation traffic

**Receiver smoothing:** concealment and LiveLink's own buffering add some delay in exchange for smooth motion. See [Receiver Source Settings](#receiver-source-settings) and LiveLink's buffer settings on the source.

### Memory Optimization

**Limit Queue Sizes:**

The sender-side queues hold frames while the network or the receiver is slower than the sender. Smaller queues use less memory and drop or refuse frames sooner:

| Transport | Option | Default |
|-----------|--------|---------|
| Loopback | `loopback.maxqueue` (**Queue Capacity**) | 64 frames |
| TCP | `tcp.maxqueue` | 4194304 bytes |
| NNG | `nng.qmax` (**Queue Capacity (MiB)**) | 4 MiB |
| MoQ | `queue_bytes` (**Queue Capacity (MiB)**) | 8 MiB |

A byte queue smaller than one frame refuses every frame; see [Frames are larger than the transport accepts](#frames-are-larger-than-the-transport-accepts).

**Subject Cleanup:**
- Subjects idle for longer than **Inactive Subject Timeout Seconds** (default 5 s) are cleared, not removed, so their LiveLink settings survive a pause
- The receiver forgets the stream state of senders idle that long

### CPU Optimization

**Reduce Serialization Cost:**
- Lower capture rate
- Fewer curves
- Simpler skeletons

**Threads:**
- With `o3d.Sender.AsyncPipeline` at 1 (the default), the sender filters, serializes and sends pose frames on a worker task, not on the game thread
- The TCP, UDP, NNG and MoQ senders write to the network from a worker thread of their own

### Monitoring Performance

**Transport Statistics:**

In Blueprint, **Get Transport Stats** on the sender component returns the counters listed in [Connection State and Transport Stats](#connection-state-and-transport-stats). In C++:
```cpp
const FO3DBroadcastTransportStats Stats = Sender->GetTransportStats();
UE_LOG(LogTemp, Log, TEXT("Sent %lld frames, %lld bytes, dropped %lld"),
       Stats.FramesSent, Stats.BytesSent, Stats.DroppedFrames);
```

**Console commands:** `o3d.DumpMetrics` prints the plugin's performance metrics, `o3ds.Sender.DumpStats` the serializer's per-subject statistics, and `o3d.Sender.DumpPipelineStats` each sender's pipeline (queue, drops, worker time, capture-to-send latency). See [Console Variables and Commands](#console-variables-and-commands).

**Receiver status:** the LiveLink panel shows each Open3DStream source's status line, for example "Receiving via UDP" or "No data received". The [Source Status](#source-status) table lists them.

---

## Troubleshooting

### Common Issues

#### "Subject not appearing in LiveLink"

**Symptoms:** Sender is capturing but receiver shows no subjects

**Solutions:**
1. **Check the sender:**
   - **Auto Create Transport** is ticked, and capture runs (Play In Editor or a game, not the editor viewport)
   - **On Sender Error** or the Output Log (`LogO3DSenderComponent`) says why the transport did not start

2. **Check transport configuration:**
   - The sender's **Transport Name** and the source's **Transport** are the same
   - The options point at each other: the same **Channel Name** (Loopback), host and port (TCP, UDP, NNG), mode (NNG), or relay URL, track namespace and track name (MoQ). See [Transport Modules](#transport-modules)

3. **Check network connectivity:**
   - TCP, UDP, NNG: the listening machine's firewall allows the port; see [Ports and Firewalls](#ports-and-firewalls)
   - WebRTC: the server URL and the token; see the add-on's USER_GUIDE
   - Test with Loopback first to isolate network issues

4. **Check the LiveLink source status:**
   - The [status line](#source-status) in the Live Link panel says what the receiver sees
   - Try removing and re-creating the source

5. **Enable debug logging:**
   ```
   log LogO3DReceiverSource Verbose
   ```
   See [Debug Logging](#debug-logging) for the transport's own category.

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
   - Verify **Receive Mode** and **Stream Label Filter** (**Mix (o3ds:mix)** plays only streams labelled `o3ds:mix`; use **Any Stream** or **Subject (LiveLink)** for a sender's audio)
   - Check **Gain** is not zero
   - With **Auto Activate** off, call **Play**

4. **Check audio device:**
   - Windows Sound Settings → Unreal Engine not muted
   - For Input mode: Windows allows desktop apps to use the microphone (Windows privacy settings)
   - For Mix mode: Game actually producing audio

5. **Debug logging:** set `o3ds.Sender.Audio.Debug 1` on the sender, and `o3ds.Receiver.Audio.Debug 1` and `o3ds.RemoteAudio.Debug 1` on the receiver.

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
   - LAN: TCP or UDP (UDP has the lowest latency)
   - Internet: WebRTC (add-on) or MoQ (Experimental, needs a relay); expect more latency than on a LAN

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
   - The network or the receiver is slower than the sender
   - Raise the transport's queue capacity (`loopback.maxqueue`, `tcp.maxqueue`, `nng.qmax` or `queue_bytes`; see [Memory Optimization](#memory-optimization)), or reduce the data rate

4. **Check transport stats:**
   - **Get Transport Stats** on the sender: a growing **Dropped Frames** count means the transport could not take frames
   - `o3d.Sender.DumpPipelineStats` shows frames the sender's own pipeline dropped before the transport (`o3d.Sender.PipelineDepth`)

#### Frames are larger than the transport accepts

**Symptom:** the Output Log on the sender shows

```
Subject '<name>': the <N>-byte frame is larger than the transport accepts and was not sent (<M> more since the last warning). Reduce the skeleton or raise the transport's queue capacity.
```

The frame does not fit the transport's send queue, or is larger than the largest message the transport carries. Nothing of that subject reaches the receiver while its frames stay that large.

**Solutions:**
- Raise the queue capacity: `tcp.maxqueue` (TCP), **Queue Capacity (MiB)** (NNG, MoQ). See [Memory Optimization](#memory-optimization).
- Make frames smaller: capture a mesh with fewer bones, or leave curves out with **Exclude Curve Patterns**.
- On the receiver, TCP accepts frames up to `tcp.maxframe` (4194304 bytes) and UDP reassembles messages up to `udp.maxframe` (4194304 bytes).
- WebRTC sends a frame of at most 15,000 bytes, and its own log says `payload size (N bytes) exceeds maximum (15000 bytes)`. See "Payload Too Large" in the add-on's USER_GUIDE.

#### "Invalid options" or InvalidConfig

**Symptom:** the source status is **Invalid options: *reason***, **Create Source** is disabled with a reason, or the sender logs `Sender transport '<name>' not started: InvalidConfig: <reason>` (and fires **On Sender Error**).

A transport option is out of range or malformed, for example a port above 65535. The reason says which option. Fix it in the panel, in the [project-wide defaults](#project-wide-transport-defaults), or in the options you pass from Blueprint or C++. The ranges are in the [Transport Options Reference](#transport-options-reference).

#### Firewall and ports

**Symptom:** Loopback works, and the same setup across two machines does not; the receiver stays at "Waiting for data" or "Reconnecting".

Open the port on the machine that listens: the sender for TCP (17700), the receiver for UDP (17800), and the listening end for NNG (6000, 7000 or 8000 by mode). MoQ and WebRTC need outbound access only. The [Ports and Firewalls](#ports-and-firewalls) table has the details.

#### "WebRTC is not in the transport list" or "WebRTC connection fails"

1. **Is the add-on installed and enabled?** WebRTC comes from the free Open3DBroadcastWebRTC add-on ([WebRTC Transport (free add-on)](#webrtc-transport-free-add-on)). Without it, "WebRTC" is not in the transport pickers.
2. **Does the add-on match your Open3DBroadcast release?** Search the Output Log for `WebRTC transport not registered`. The message says whether the transport API version differs (install the matching add-on build) or `livekit_ffi.dll` could not be loaded.
3. **Connection problems** (server URL, tokens, TURN): see the troubleshooting section of the add-on's USER_GUIDE.

### Debug Logging

Enable verbose logging for troubleshooting with the `log` console command, for example `log LogO3DReceiverSource Verbose`. The useful categories:

| Area | Categories |
|------|------------|
| Sender component | `LogO3DSenderComponent`, `LogO3DSenderAudio`, `LogO3DSenderSerializer` |
| Receiver source | `LogO3DReceiverSource`, `LogO3DReceiverAudio` |
| Loopback | `LogO3DLoopbackTransport` |
| TCP and UDP | `LogSocketsTcpSender`, `LogSocketsTcpReceiver`, `LogSocketsUdpSender`, `LogSocketsUdpReceiver`, `LogOpen3DTransportSocketsModule` |
| NNG | `LogO3DNngSender`, `LogO3DNngReceiver`, `LogOpen3DTransportNNGModule` |
| MoQ | `LogO3DMoQSender`, `LogO3DMoQReceiver`, `LogOpen3DTransportMoQModule` |
| WebRTC (add-on) | `LogO3DWebRTCSender`, `LogO3DWebRTCReceiver`, `LogO3DWebRTCTokenManager`, `LogOpen3DTransportWebRTCModule` |
| Metrics | `LogO3DPerformanceMetrics` |

The WebRTC categories exist only when the Open3DBroadcastWebRTC add-on is installed.

**In DefaultEngine.ini:**
```ini
[Core.Log]
LogO3DSenderComponent=Verbose
LogO3DReceiverSource=Verbose
```

### Console Variables and Commands

Set a variable in the console (for example `o3ds.Receiver.DebugParse 1`), or in the `[SystemSettings]` section of `DefaultEngine.ini`. The debug variables log a lot; turn them off again when you are done.

**Receiver**

| Name | Default | What it does |
|------|---------|--------------|
| `o3ds.Receiver.DebugParse` | 0 | 1: log when incoming packets are parsed |
| `o3ds.Receiver.DropOutOfOrder` | 1 | 1: drop frames whose time is older than the last applied frame's |
| `o3ds.Receiver.SilenceResetSeconds` | 2.0 | After this many seconds without packets, reset the frame-ordering state. 0: never |
| `o3ds.Receiver.TimestampJumpResetSeconds` | 1.0 | When a frame's time goes back by more than this many seconds, reset the frame-ordering state. 0: never |
| `o3ds.Receiver.Audio.Debug` | 0 | 1: log the audio frames the receiver publishes |
| `o3ds.RemoteAudio.Debug` | 0 | 1: log what the O3D Remote Audio Component receives and plays |

**Sender**

| Name | Default | What it does |
|------|---------|--------------|
| `o3ds.Sender.DebugPose` | 0 | 1: log every pose frame |
| `o3ds.Sender.DebugCurves` | 0 | 1: log the curves of every frame |
| `o3ds.Sender.DebugSerialize` | 0 | 1: log serialization |
| `o3ds.Sender.DebugStats` | 0 | 1: log serializer statistics for every frame |
| `o3ds.Sender.OnScreen` | 0 | 1: show on-screen messages when a sender component's state changes |
| `o3ds.Sender.Audio.Debug` | 0 | 1: log audio capture |
| `o3ds.Sender.Audio.WarnFailures` | 1 | 1: warn when the transport refuses audio frames |
| `o3d.Sender.AsyncPipeline` | 1 | 1: filter, serialize and send pose frames on a worker task. 0: on the game thread. Read when capture starts |
| `o3d.Sender.PipelineDepth` | 2 | Pose frames that may wait for a sender's worker (1 to 8). When another arrives, the oldest waiting one is dropped |
| `o3ds.Loopback.Audio.Debug` | 0 | Loopback audio logging: 0 off, 1 basic, 2 verbose |

**Commands**

| Command | What it does |
|---------|--------------|
| `o3ds.Sender.DumpStats` | Logs each subject's serialization statistics |
| `o3d.Sender.DumpPipelineStats` | Logs each sender's pipeline statistics: queue, drops, worker time, capture-to-send latency. Available once a sender has started capture |
| `o3d.Sender.Capture.Start [file]` | Records every serialized pose frame the senders send to a `.o3dscap` file. A relative path goes under `Saved/O3DCaptures` |
| `o3d.Sender.Capture.Stop` | Stops that recording |
| `o3d.Sender.Audio.RefreshDevices` | Enumerates the audio capture devices again and logs them |
| `o3d.DumpMetrics` | Logs the plugin's performance metrics: the default runtime context, then each named one |
| `o3d.ResetMetrics` | Resets the performance metrics in every runtime context |

---

## Known Limitations

- **Unreal Engine 5.7 and 5.8, Win64 only**, for editor and game targets. Server and Program targets are not supported.
- **No sample content.** No map or assets ship with the plugin; the [Quick Start](#quick-start) builds a working setup from the Third Person template.
- **MoQ is Experimental.** It implements draft-ietf-moq-transport-07 and needs a relay that speaks draft-07. Its options and behaviour can change between releases.
- **WebRTC is a separate add-on**, and each add-on build works only with the Open3DBroadcast release it was built for.
- **Residual coding needs a reliable, ordered transport** (Loopback, TCP, NNG Pair or Push/Pull, WebRTC by default). On UDP, NNG Pub/Sub, MoQ, or WebRTC with `webrtc.prefer_lossy`, the sender sends without it and logs a warning.
- **UDP multicast is IPv4 only**, and crosses routers only where they forward multicast. Many Wi-Fi networks and managed switches drop or rate-limit it.
- **TCP serves one receiver at a time** per sender.
- **No encryption or authentication on TCP, UDP and NNG.** Use them on networks you trust.
- **One LiveLink client per process.** Subject names are shared by every receiver source in a process, PIE clients included; keep them distinct (see [Separate Receivers](#separate-receivers-runtime-contexts)).

## Privacy

- **Microphone:** the sender opens a microphone only when **Enable Audio** is ticked and **Audio Capture Mode** is **Input (Microphone)**. **Enable Audio** is off by default. In Mix mode it captures the game's own audio, not a microphone.
- **Who hears it:** captured audio, like the animation, goes to every receiver of the stream: every receiver on the Loopback channel, the receiver connected to a TCP sender, every machine that receives the UDP datagrams (with broadcast, every machine on the subnet that listens on the port), every NNG subscriber, every subscriber of the MoQ track on the relay, and the participants of the WebRTC room.
- **Transport security:** TCP, UDP and NNG are neither encrypted nor authenticated, so anyone on the network path can read the stream. Use them on networks you trust.
- **Control** is off by default on receivers: a client accepts control only when its project turns it on (see [Enabling Control on a Client](#enabling-control-on-a-client)).
- **Credentials** (for example a WebRTC token) are never saved in a level, a Blueprint or a LiveLink preset; see [Transport Configuration](#transport-configuration).

If your project captures players' or performers' voices, tell them, and follow the privacy rules that apply to you.

## Updating and Removing the Plugin

- **Updating:** close the editor and replace the plugin: through the launcher for an engine install, or by replacing the `Plugins/Open3DBroadcast` folder. If you use the WebRTC add-on, update it to the same version; it only works with the matching Open3DBroadcast release, and logs `WebRTC transport not registered: ...` otherwise.
- **Removing:** remove Open3DBroadcast components and LiveLink sources from your assets first, or they load as missing. Then disable the plugin in **Edit → Plugins**, close the editor, and delete the folder (or uninstall it from the engine in the launcher). Remove the WebRTC add-on too: it depends on Open3DBroadcast.
- **Project settings** you set under **Project Settings > Plugins** (**Open3DBroadcast**, **Open3DBroadcast Control**) stay in your project's `Config/DefaultGame.ini` until you delete those sections.

---

## Advanced Topics

### Custom Transport Implementation

A transport is a module that implements the transport interfaces and registers itself. The interfaces and the registry are in the `Open3DShared` module, under `Public/Transport/`.

**Interfaces:**
- `IOpen3DSender` (`O3DSenderInterface.h`): takes serialized frames, audio and control from a sender component.
- `IOpen3DReceiver` (`O3DReceiverInterface.h`): hands received frames, audio and control to a receiver source.

**Registration:** fill one `FO3DTransportDescriptor` per transport name and register it with `FO3DTransportRegistry` when the module starts. Keep the returned `FO3DTransportRegistration`: resetting it in `ShutdownModule` unregisters the transport and stops every instance still running.

```cpp
#include "Transport/O3DTransportRegistry.h"

void FMyTransportModule::StartupModule()
{
    FO3DTransportDescriptor Descriptor;
    Descriptor.Name = TEXT("MyTransport");          // shown in the transport pickers
    Descriptor.OwningModule = TEXT("MyTransport");
    Descriptor.CreateSender = []() { return MakeShared<FMySender, ESPMode::ThreadSafe>(); };
    Descriptor.CreateReceiver = []() { return MakeShared<FMyReceiver, ESPMode::ThreadSafe>(); };
    // Optional: ConfigureSender / ConfigureReceiver (options to config), GetCapabilities,
    // and SenderOptions / ReceiverOptions (the option schema the panels show).

    Registration = FO3DTransportRegistry::Get().Register(MoveTemp(Descriptor));
}

void FMyTransportModule::ShutdownModule()
{
    Registration.Reset();
}
```

A descriptor carries the transport API version it was compiled with (`O3D_TRANSPORT_API_VERSION`); the registry refuses one built against another version, so rebuild a custom transport for each Open3DBroadcast release. The plugin's own transports (`Source/Open3DTransport*/`) are complete examples; the Loopback module is the smallest.

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

Each component runs its own transport and is identified by its subject name. Which receiver setups hear them all is in [Multiple Subjects](#multiple-subjects).

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

### Processing Audio Before Sending

The sender has no hook for processing captured audio. To send processed game audio, route it into a submix (with the submix's effects) and set that submix as **Submix to Tap** in Mix mode.

### Bandwidth Estimation

A rough estimate, without message headers. Measure the real figure with **Get Transport Stats** (**Bytes Sent**) or `o3ds.Sender.DumpStats`.

**Uncompressed (no filtering):**
```
Skeleton: 100 bones × 7 floats (Transform) × 4 bytes = 2.8 KB
Curves: 50 curves × 4 bytes = 0.2 KB
Frame size: ~3 KB

At 60 Hz: 3 KB × 60 = 180 KB/s = 1.44 Mbps
```

**With curve filtering (half the curve values left out):**
```
Frame size: 2.8 KB + 0.1 KB = ~2.9 KB
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
- [ ] Test firewall and NAT scenarios (ports for TCP, UDP and NNG; outbound access for MoQ and WebRTC)
- [ ] Prepare fallback configurations

---

## FAQ

**Does the plugin include sample content or a demo map?**
No. Follow the [Quick Start](#quick-start); it needs only the Third Person template.

**Which transport should I use?**
Loopback to test in one editor. TCP for one receiver on a LAN, UDP for the lowest latency or multicast to several receivers, NNG Pub/Sub for several receivers on a LAN. Across networks, WebRTC (free add-on) or MoQ (Experimental, needs a relay). See [Transports at a Glance](#transports-at-a-glance).

**Can the sender and the receiver run in the same editor?**
Yes. Use Loopback, as in the Quick Start, or TCP or UDP with the default host `127.0.0.1`.

**Does it work on Mac, Linux, consoles or dedicated servers?**
No. Win64 and Unreal Engine 5.7 and 5.8 only, editor and game targets.

**Does it stream audio?**
Yes, on every transport. Tick **Enable Audio** on the sender and on the LiveLink source, and add an O3D Remote Audio Component on the receiving side. See [Audio Streaming](#audio-streaming).

**Is the microphone recorded?**
Only when **Enable Audio** is ticked and **Audio Capture Mode** is **Input (Microphone)**. See [Privacy](#privacy).

**Can I set everything up from Blueprint at runtime?**
Yes: the sender's transport nodes, **Create Open3DStream LiveLink Source** for the receiver, and **Set Transport Secret** for credentials. See the [Blueprint API Reference](#blueprint-api-reference).

**Why does my sender capture but nothing arrives?**
**Auto Create Transport** is off by default; tick it. Then see [Subject not appearing in LiveLink](#subject-not-appearing-in-livelink).

**The sender and the receiver use different skeletons. What do I do?**
Retarget on the receiver; see [Retargeting Animation](#retargeting-animation).

**Can other tools read the stream?**
The stream uses the Open3DStream protocol; its byte layout is in [docs/wire-format.md](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/wire-format.md).

**Where do I report a bug?**
[GitHub Issues](https://github.com/lifelike-and-believable/Open3DBroadcast/issues).

---

## Additional Resources

### Documentation

- **Plugin README**: [README.md](README.md) - requirements, installation, ports and known limitations
- **Transport Comparison**: [Transport_Module_Comparison.md](Transport_Module_Comparison.md) - how the transports differ, for integrators
- **Transport READMEs** (in the GitHub repository, not in the Fab package): [Loopback](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportLoopback/README.md), [TCP and UDP](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportSockets/README.md), [NNG](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportNNG/README.md), [MoQ](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/ProjectSandbox/Plugins/Open3DBroadcast/Source/Open3DTransportMoQ/README.md)
- **WebRTC Guide**: [Open3DBroadcastWebRTC USER_GUIDE](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/ProjectSandbox/Plugins/Open3DBroadcastWebRTC/USER_GUIDE.md) - setup of the free WebRTC add-on
- **Wire format**: [docs/wire-format.md](https://github.com/lifelike-and-believable/Open3DBroadcast/blob/develop/docs/wire-format.md)

### Support

- **GitHub Issues**: [report bugs and request features](https://github.com/lifelike-and-believable/Open3DBroadcast/issues)
- **Code Examples**: the plugin's source, in `Source/` of the plugin folder; the transports in `Source/Open3DTransport*/` show how a transport is built

### Next Steps

1. **Start simple**: Use Loopback transport for learning
2. **Experiment**: Try different transports and settings
3. **Optimize**: Tune for your specific use case
4. **Scale**: Move to WebRTC (add-on) or MoQ (Experimental) for streams between networks
5. **Customize**: Implement custom transports if needed

---

**Happy Streaming!**
