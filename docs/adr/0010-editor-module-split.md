# 0010: Editor module split

- **Status:** Accepted (maintainer sign-off 2026-09-29)
- **Accepted with defaults:** every open question below was accepted with the recommended default given next to it. Needs-verification items stay open and are resolved in the implementing WPs; a result that invalidates a default is handled by a superseding ADR.
- **Date:** 2026-09-29
- **Plan decision:** D9 in [`plugin-hardening-and-fab-readiness.md`](../roadmap/plugin-hardening-and-fab-readiness.md) §3
- **Related:** [ADR 0001](0001-platform-scope-first-fab-release.md) (module keys, Receiver-on-Server kept possible), [ADR 0002](0002-webrtc-and-moq-in-first-fab-release.md) (WebRTC add-on), [ADR 0004](0004-credentials-and-secret-transport-options.md) (secret widgets, per-user settings class), [ADR 0006](0006-test-module-layout-and-fakes.md) (test module), [ADR 0007](0007-transport-abstraction-and-registry.md) (D4: descriptor and option schema); feeds WP-F7, WP-F6, WP-U1, WP-U3, WP-S9e, WP-F11

**Recommendation in one line:** add one `Open3DBroadcastEditor` module (Type `Editor`, `LoadingPhase: PostEngineInit`, `PlatformAllowList: ["Win64"]`) that owns every detail customization and Slate panel. Runtime transport modules register only **data**: the ADR 0007 option schema, which includes their secret keys. The editor module builds generic, undoable panels from that data. A transport whose UI the schema cannot express may ship its own small Editor module and register a custom panel through an exported hook. The LiveLink source factory stays in the runtime Receiver module and asks the editor module for its panel.

## Context

**Editor code in Runtime modules today (FAB-7, SND-34, TRB-46).** All eight modules are `Type: Runtime` (`Plugin/Open3DBroadcast.uplugin:21-62`).

| Module | Editor code | Editor dependencies |
|---|---|---|
| Open3DSender | `FO3DSenderComponentCustomization` (`Private/O3DSenderComponentCustomization.cpp`, 659 lines), registered in `Private/Open3DSenderModule.cpp:4-8`, `:23-45` | `Open3DSender.Build.cs:79-89`: Slate, SlateCore, PropertyEditor, **EditorStyle**, InputCore under `bBuildEditor` |
| Open3DReceiver | `SO3DReceiverSourceFactoryPanel` (`Private/O3DReceiverSourceFactory.cpp:48-526`); `SO3DTransportConfigPanelBase`, public and exported, not editor-guarded (`Public/O3DTransportConfigPanelBase.h:15`) | Slate and SlateCore **unconditional** (`Open3DReceiver.Build.cs:64-65`); PropertyEditor, InputCore under `bBuildEditor` (`:69-76`) |
| Sockets | four panel files, 1,136 lines (`Private/Shared/SocketsTransport{Tcp,Udp}{Sender,Receiver}Widget.cpp`) | `Open3DTransportSockets.Build.cs:45-53` |
| Loopback | panels in `Open3DTransportLoopbackModule.cpp:30-123`, `:131-267` (of 362 lines) | `Open3DTransportLoopback.Build.cs:32-40` |
| NNG | panels in `Open3DTransportNNGModule.cpp:117-836` (of 1,025 lines) | `Open3DTransportNNG.Build.cs:68-76`; InputCore, ApplicationCore, HTTP unconditional (`:61-63`) |
| MoQ | panels in `Open3DTransportMoQModule.cpp:101-545` (of 729) | `Open3DTransportMoQ.Build.cs:143-151` |
| WebRTC | panels in `Open3DTransportWebRTCModule.cpp:77-609` (of 789) | `Open3DTransportWebRTC.Build.cs:90-97` |

**How panels are registered today.** Each transport fills `BuildTransportWidget` inside the customization struct, and that member exists only `#if WITH_EDITOR` (`Open3DSender/Public/O3DSenderTransportCustomization.h:17-23`; `Open3DReceiver/Public/O3DReceiverTransportCustomization.h:17-19`). A struct whose layout depends on `WITH_EDITOR` and crosses module boundaries is the ODR hazard SND-34 names. SND-34 cites `:249-255` for this; the file has 32 lines today.

**Panel defects to fix while moving (TRB-45, SND-35).**
- Panels keep raw `UO3DSenderComponent*` or `UO3DReceiverSettingsObject*` (`SocketsTransportEditorWidgets.h:20-27`).
- `Construct()` writes defaults into the component, for example `SetBindHost` and `SetPort` (`SocketsTransportTcpSenderWidget.cpp:30-47`).
- Spin boxes notify on every drag tick.
- No edit is wrapped in `FScopedTransaction` (a repository grep finds none).
- Switching transports clears every option (`O3DSenderComponentCustomization.cpp:364-375`).

**The LiveLink source factory.** `UO3DReceiverSourceFactory : ULiveLinkSourceFactory` (`Open3DReceiver/Public/O3DReceiverSourceFactory.h:13-23`) overrides `BuildCreationPanel`, which returns `TSharedPtr<SWidget>` and is guarded internally (`O3DReceiverSourceFactory.cpp:538-545`), and `CreateSource(ConnectionString)` (`:548`). `CreateSource` is also what applying a LiveLink preset in a game calls, so the factory class must stay loadable at runtime (**needs-verification**, Q2).

**Development diary (SHR-27).** `o3d.ProfileGuide` prints internal investigation notes at Warning (`Open3DShared/Private/O3DPerformanceMetrics.cpp:545-588`). `o3d.DumpMetrics` and `o3d.ResetMetrics` (`:523-539`) are useful in PIE and development game builds.

**Constraints from accepted ADRs.**
- 0001: every module carries `PlatformAllowList`; an Editor module needs no target list; Receiver runtime code must not depend on editor-only modules (Receiver-on-Server).
- 0004: the per-user settings class (R3) and the secret widgets move to this module; secret fields open empty and show a status line.
- 0006: `Open3DBroadcastTests` is `Editor` type and depends on this module once it exists.
- 0002: the WebRTC add-on must get its panels without editing the main plugin.

## Decision drivers

1. Runtime modules compile with no Slate, PropertyEditor or UnrealEd dependency (WP-F7 acceptance), which also keeps 0001's Server option open.
2. The WebRTC add-on (and any future transport) gets a correct, undoable UI without owning Slate code.
3. One implementation of weak pointers, transactions and commit-only notifications, instead of seven.
4. Few modules.
5. No ODR hazards from `WITH_EDITOR` struct layouts.

## Options considered

### Module layout
- **M1. One `Open3DBroadcastEditor` module (chosen).** Sender and Receiver panels share the option-row widgets, secret widgets and schema renderer.
- **M2. `Open3DSenderEditor` plus `Open3DReceiverEditor` (SND-34's suggestion).** Two modules sharing a third helper, or duplicating it. No benefit, because the plugin always ships both sides.
- **M3. One Editor module per transport.** Five or more modules, each with its own Slate code. Rejected as the default; kept as an escape hatch (below).

### How transport panels reach the editor
- **P1. Runtime registers data, the editor builds generic widgets (chosen).** Uses ADR 0007's `FO3DTransportOptionSchema`: typed fields, defaults, ranges, enums, `VisibleWhen`, secrets. The existing panels are forms over namespaced string options, which the schema covers. Transports stay headless.
- **P2. Runtime modules keep registering widget factories, guarded by `WITH_EDITOR`.** This is today's model: Slate stays in runtime modules and the layout-varying struct stays. Rejected.
- **P3. Each transport has its own Editor module.** Correct, but it multiplies modules and Slate code (M3).

Chosen: **P1, with P3 as an opt-in escape hatch** through an exported registration API in the editor module.

## Decision

**1. The module.** Add `Plugin/Source/Open3DBroadcastEditor/`:
```json
{ "Name": "Open3DBroadcastEditor", "Type": "Editor",
  "LoadingPhase": "PostEngineInit", "PlatformAllowList": [ "Win64" ] }
```
- Private dependencies: `Core`, `CoreUObject`, `Engine`, `Slate`, `SlateCore`, `InputCore`, `PropertyEditor`, `UnrealEd` (for `FScopedTransaction`), `LiveLinkInterface`, `Open3DShared`, `Open3DSender`, `Open3DReceiver`.
- No `EditorStyle`: use `FAppStyle` (**needs-verification** that 5.7 has removed `EditorStyle` and `FAppStyle` is the replacement, Q3).
- It **ships in the Fab package**. It is not in the exclusion manifest; `Open3DBroadcastTests` is.
- It exports `OPEN3DBROADCASTEDITOR_API` for the escape hatch and for tests.

**2. What moves, and what stays.**

| Item | Destination |
|---|---|
| `FO3DSenderComponentCustomization` and its `RegisterCustomClassLayout` (`Open3DSenderModule.cpp:23-45`) | Editor module `StartupModule` |
| `SO3DReceiverSourceFactoryPanel`, `SO3DTransportConfigPanelBase` | Editor module |
| Sockets, Loopback, NNG and MoQ panels | **deleted**; replaced by the schema renderer |
| WebRTC panels | deleted in the add-on; replaced by its schema (Q4) |
| 0004's per-user secret settings class (R3) and secret widgets | Editor module |
| `PostEditChangeProperty`, `CanEditChange` and similar UObject hooks | **stay** in the runtime classes under `WITH_EDITOR`; they are part of the UObject |
| `UO3DReceiverSourceFactory` | **stays** in Open3DReceiver (runtime) |
| `UOpen3DBroadcastSettings : UDeveloperSettings` (UX-2, WP-U1) | runtime (Open3DShared), so packaged games read defaults |

**3. LiveLink creation panel.** Open3DReceiver exports `O3DReceiver::SetSourceFactoryPanelBuilder(TFunction<TSharedPtr<SWidget>(FOnLiveLinkSourceCreated)>)`. `UO3DReceiverSourceFactory::BuildCreationPanel` calls it if set and returns `nullptr` otherwise. The editor module sets it in `StartupModule` and clears it in `ShutdownModule`.
- If `LiveLinkInterface`'s `LiveLinkSourceFactory.h` includes Slate headers, Receiver keeps `SlateCore` as a private dependency for the `SWidget` type only (**needs-verification**, Q2). `Slate` is removed from `Open3DReceiver.Build.cs:64`.

**4. Registration without depending on the editor module.**
- **Default (P1).** A transport registers its descriptor (ADR 0007 item 4) with `SenderOptions` and `ReceiverOptions` schemas and nothing else. The editor module's `SO3DTransportOptionsPanel` renders any schema:
  - one row per visible field, typed widget per `Type`;
  - enum combo boxes;
  - `VisibleWhen` evaluated live;
  - `Default` shown as hint text;
  - `Secret` fields as empty password boxes with 0004's status line, Clear button and "Remember on this machine" box.

  The same panel is used in the sender Details panel and in the LiveLink creation panel. The editor module rebuilds panels on the registry's `OnTransportsChanged`, so an add-on enabled later appears without restarting the Details panel.
- **Cross-field validation.** ADR 0007's schema gains an optional `TFunction<TArray<FText>(const FO3DTransportOptionsView&)> Validate` (for example UDP MTU ≤ `udp.maxdatagram`, TRB-45). The panel shows its messages inline, and the receiver factory disables "Create Source" while any are present (RCV-17).
- **Escape hatch (P3).** `IO3DBroadcastEditorModule::RegisterCustomTransportPanel(FName Transport, EO3DTransportRole, FO3DCustomPanelBuilder)` replaces the generic panel for one transport. Only a transport's **own Editor module** calls it (Type `Editor`, depending on `Open3DBroadcastEditor`). No runtime module may.
- **WebRTC add-on:** uses P1. If it ever needs P3, it adds `Open3DBroadcastWebRTCEditor` (Type `Editor`, Win64) next to `Open3DBroadcastWebRTCTests` (ADR 0006).

**5. Panel behaviour (TRB-45, SND-35), implemented once in the renderer.**
- The panel edits through an `IO3DOptionTarget` adapter holding a `TWeakObjectPtr` to the sender component or receiver settings object. Every access checks validity, and the panel shows "Object no longer valid" instead of crashing.
- `Construct()` never writes to the target. Defaults are hints until the user commits a value.
- Each committed edit opens `FScopedTransaction`, calls `Modify()` and writes one key. Undo and redo restore the option map.
- Spin boxes and text boxes notify on `OnValueCommitted` only. A transport restart happens only for fields marked `bRestartOnChange`, at most once per commit.
- Changing the transport does not clear other transports' namespaced keys (ADR 0007 item 8). A "Reset transport options" button exists, inside a transaction.
- Secret values never touch the option map (0004).
- The Details panel shows a warning row when no transport is configured, or when the selected one is not registered (UX-3 part, WP-U3).

**6. Runtime module clean-up.**
- Delete the `bBuildEditor` blocks in Sender (`Open3DSender.Build.cs:79-89`), Receiver (`:69-76`), Loopback (`:32-40`), Sockets (`:45-53`), NNG (`:68-76`), MoQ (`:143-151`) and WebRTC (`:90-97`).
- Remove NNG's unconditional `InputCore` and `ApplicationCore` (`:61-62`), and `HTTP` (`:63`) if nothing but `FGenericPlatformHttp` uses it (TRB-46).
- The descriptor from ADR 0007 has no `WITH_EDITOR` members, which removes the ODR hazard.

**7. Console commands (SHR-27).**
- `o3d.ProfileGuide` and `PrintProfileGuide` are deleted (WP-F6); the text, if still useful, goes to `docs/dev/`.
- `o3d.DumpMetrics` and `o3d.ResetMetrics` stay in Open3DShared. They are useful in PIE and development game builds, which is why they are not moved to the Editor module. They are wrapped in `#if !UE_BUILD_SHIPPING`, with handlers `static` and lifetime owned by the module (WP-A6).

**8. Relation to the test module (ADR 0006).**
- `Open3DBroadcastTests` (Type `Editor`) adds `Open3DBroadcastEditor` as a private dependency.
- It tests:
  - opening each registered transport's panel leaves the package clean;
  - one commit creates exactly one transaction, and undo restores the previous value;
  - secret fields never write to `TransportOptions`;
  - the Receiver and all transport modules build with the Editor module disabled (a Game target build in the nightly, per 0006).
- Both modules are Editor-only. They differ in that the editor module ships on Fab and the test module does not.

## Consequences

- **Easier:**
  - About 2,700 lines of per-transport Slate code are deleted (NNG about 720, WebRTC about 530, MoQ about 445, Loopback about 230, Sockets about 1,140 lines of panels).
  - New transports and the add-on get a correct UI from data.
  - Runtime modules are headless, which keeps 0001's Server option open.
  - Undo works everywhere.
- **Harder:**
  - Bespoke touches in today's panels are lost unless the schema expresses them, for example NNG's URL assembly, which becomes separate host, port and mode fields that `ConfigureSender` composes. The maintainer should review the rendered panels for each transport in the WP-F7 PR (screenshots).
  - The schema is now a public, versioned API (bumps `O3D_TRANSPORT_API_VERSION` when it changes, ADR 0007).
- **Constrains:**
  - WP-F7 depends on WP-A1 PR 5 (schemas). An interim step (Implementation 1) lets WP-F7 start earlier.
  - ADR 0004's R3 class lands in this module.

## Implementation outline

1. **WP-F7a (can start before WP-A1):** create `Open3DBroadcastEditor` (Build.cs, module, `.uplugin` entry with the 0001 keys). Move `FO3DSenderComponentCustomization`, `SO3DReceiverSourceFactoryPanel` and `SO3DTransportConfigPanelBase` into it. Add the Receiver panel-builder hook. Drop `EditorStyle`. Transports temporarily keep `BuildTransportWidget` under `WITH_EDITOR`, now declared in a new editor-module header, so the dependency direction is already correct for Sender and Receiver.
2. **WP-F7b (after WP-A1 PR 5):** `SO3DTransportOptionsPanel` and `IO3DOptionTarget`, the secret widgets (with WP-S9e), validation display, transactions. Migrate Loopback, then Sockets, NNG and MoQ one per PR, each deleting that transport's panel code and Build.cs editor block.
3. **WP-F7c:** remove `BuildTransportWidget` and the `WITH_EDITOR` members from the customization shims. Add a CI grep: no runtime `*.Build.cs` names `Slate`, `PropertyEditor`, `UnrealEd`, `EditorStyle` or `ToolMenus`, with Receiver's `SlateCore` allowed only if Q2 confirms it is needed.
4. **WP-F11:** the WebRTC add-on declares its schema and deletes `Open3DTransportWebRTCModule.cpp:77-609`.
5. **WP-F6:** delete `o3d.ProfileGuide`. **WP-A6:** module-scoped metrics commands.
6. **WP-U1 / WP-U3:** project settings (runtime) and the Details warnings (editor module).

## Verification / acceptance

- A packaged **Shipping** Win64 game builds, and no plugin runtime module links Slate, PropertyEditor or UnrealEd (the WP-F7 grep plus the nightly game build from 0006).
- In the editor: every registered transport, including WebRTC from the add-on, shows a panel in the sender Details panel and in the LiveLink "Add Source" menu. Opening a panel does not mark the level dirty. Ctrl+Z reverts an option edit. Dragging a spin box restarts the transport at most once.
- Disabling the WebRTC add-on and restarting leaves every other panel working (WP-F11).
- The 0006 tests listed in Decision §8 pass.

## Open questions for the maintainer

1. `LoadingPhase: PostEngineInit` for the editor module (FAB-7's suggestion)? **Default: yes.** It must be loaded before a Details panel or the LiveLink panel opens, which PostEngineInit satisfies (**needs-verification** for 5.7).
2. **needs-verification:** must `ULiveLinkSourceFactory` subclasses be in a runtime module for LiveLink presets to create sources in a packaged game, and does `LiveLinkSourceFactory.h` force a SlateCore dependency? Default: the factory stays runtime and Receiver keeps only `SlateCore` if the header needs it.
3. **needs-verification:** in 5.7, is `EditorStyle` removed and `FAppStyle` the replacement; is `FScopedTransaction` in `UnrealEd`? Default: use `FAppStyle` and depend on `UnrealEd` from the editor module only.
4. Should the WebRTC add-on use the generic panel (P1) rather than its own Editor module? **Default: P1**; add `Open3DBroadcastWebRTCEditor` only if its token UI cannot be expressed.
5. Accept losing NNG's combined URL builder in favour of separate fields? **Default: yes**, with the composed URL shown read-only under the fields.

## References

- Findings: FAB-7, SND-34, SND-35, TRB-45, TRB-46, SHR-27; parts of UX-2 and UX-3; related RCV-17, SHR-10.
- Files: `Plugin/Open3DBroadcast.uplugin`; every `Plugin/Source/*/*.Build.cs`; `Plugin/Source/Open3DSender/Private/{Open3DSenderModule.cpp,O3DSenderComponentCustomization.cpp}`; `Plugin/Source/Open3DSender/Public/O3DSenderTransportCustomization.h`; `Plugin/Source/Open3DReceiver/Private/O3DReceiverSourceFactory.cpp`, `Public/{O3DReceiverSourceFactory.h,O3DTransportConfigPanelBase.h,O3DReceiverTransportCustomization.h}`; `Plugin/Source/Open3DTransportSockets/Private/Shared/*Widget*.cpp`, `SocketsTransportEditorWidgets.h`; `Plugin/Source/Open3DTransport{Loopback,NNG,MoQ,WebRTC}/Private/*Module.cpp`; `Plugin/Source/Open3DShared/Private/O3DPerformanceMetrics.cpp`.
- External: none fetched for this ADR (UE module-type sources are listed in ADR 0006, References).
