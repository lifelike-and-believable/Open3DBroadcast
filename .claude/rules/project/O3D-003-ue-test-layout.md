---
id: O3D-003
title: Where UE tests live and how they are named
level: SHOULD
scope: project
paths: ["ProjectSandbox/Plugins/**/*.cpp", "ProjectSandbox/Plugins/**/*.h"]
verified-by: [ci]
check: "ci: UE build and tests (Run-AutomationTests.ps1 -TestFilter Open3DBroadcast)"
targets-failure: convention-drift
observed-on: []
rationale: ADR 0006 keeps tests out of Runtime modules and the CI filter finds them by name; replaces UE-007.
---
Put UE automation tests in `Source/Open3DBroadcastTests/Private/<Area>/` (the WebRTC add-on's in `Source/Open3DTransportWebRTC/Private/Tests/`), never in a Runtime module. Name them `Open3DBroadcast.<Area>.<Unit>.<Case>` and wrap them in `#if WITH_DEV_AUTOMATION_TESTS`. Reach internals through the owning module's `Public/Testing/*.h`. Tests that need the internet register only when `O3DB_NETWORK_TESTS=1`. Decide pass or fail from the report `Run-AutomationTests.ps1` reads, not the editor's exit code.
