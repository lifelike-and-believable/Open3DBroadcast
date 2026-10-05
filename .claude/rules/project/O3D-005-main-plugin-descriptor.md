---
id: O3D-005
title: Fab descriptor requirements for the main plugin
level: SHOULD
scope: project
paths: ["**/Open3DBroadcast.uplugin"]
verified-by: [review]
targets-failure: project-decision
observed-on: []
rationale: The main plugin is published on Fab and the WebRTC add-on is not, so FAB-001 applies to the main plugin's descriptor only; replaces FAB-001.
---
In `Open3DBroadcast.uplugin`, keep `EngineVersion` at `5.7.0`, give every module `"PlatformAllowList": [ "Win64" ]` (ADR 0001), and depend only on plugins that ship with the engine. Add `FabURL` once the Fab listing exists. These rules do not apply to `Open3DBroadcastWebRTC.uplugin`: the add-on is distributed from the website to registered plugin users and depends on Open3DBroadcast.
