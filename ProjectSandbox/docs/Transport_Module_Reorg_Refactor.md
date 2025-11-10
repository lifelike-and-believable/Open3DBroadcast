# Transport Module Reorganization & Refactor Plan

> Authoritative design document for splitting transport implementations (TCP/UDP, NNG, WebRTC/LiveKit) into independently buildable Unreal modules while preserving existing UX and protocol behavior. Changes MUST follow `.github/copilot-instructions.md` (no blocking game thread, deterministic behavior, do not mutate existing FlatBuffers schema unless explicitly versioned).

> The original Open3DStream codebase is located at **/plugins/unreal/Open3DStream** in this workspace.

---
## 0. Pre‑refactor Readiness & Plugin Strategy

Before touching code, line up the following to minimize churn and avoid file deletion/rename pitfalls (especially on Windows):

- Branching
	- Create `feature/transport-modules` from `develop`.
	- Protect `develop` during migration window; no large unrelated merges.

- Baseline & CI
	- Record current quick build time and test results (attach to PR).
	- Ensure CI can build Editor target with `ProjectSandbox` and run unit tests.

 - New Plugin Strategy (to avoid destructive moves)
 	- Create ONE new top‑level plugin and migrate code into it rather than mass moving within the existing plugin:
		- Create `Plugins/Open3DBroadcast/Open3DBroadcast.uplugin` and host ALL modules there:
			- `Open3DSender` (sender orchestration, capture components)
			- `Open3DReceiver` (receiver orchestration, LiveLink integration)
 			- `Open3DShared` (shared utilities & protocol helpers)
 			- `Open3DTransportSockets`, `Open3DTransportNNG`, `Open3DTransportWebRTC` (transport modules)
		- Leave the existing `Plugins/Open3DStream` plugin untouched (no edits to its `.uplugin`).
		- Control which plugin loads via `ProjectSandbox.uproject` (disable `Open3DStream`, enable `Open3DBroadcast`).
		- Rationale: avoids rename/deletion thrash and keeps the old plugin pristine while enabling per-project gating.
    

- Redirectors (if class/module names change or move across plugins)
	- Add Core Redirects in `Config/DefaultEngine.ini`:
		- `+ActiveClassRedirects=(OldName="/Script/Open3DStream.OldClass",NewName="/Script/Open3DBroadcast.NewClass")`
		- `+ActiveStructRedirects=(OldName="/Script/Open3DStream.OldStruct",NewName="/Script/Open3DBroadcast.NewStruct")`
		- `+ActiveEnumRedirects=(OldName="/Script/Open3DStream.OldEnum",NewName="/Script/Open3DBroadcast.NewEnum")`
	- For Blueprint assets/content path moves, use Asset Redirectors if any content resides under plugin Content.

- Build System
	- Add empty skeleton modules first (with `StartupModule/ShutdownModule` only) and get them compiling in CI.
	- Introduce build flags in a single PR before moving code.

- Runtime Safety
	- Ensure both old and new plugins can coexist temporarily (old disabled by default) to support phased testing.

- Documentation & Versioning
	- Prepare CHANGELOG section describing plugin split and flags (to be filled post‑migration).
	- Update README paths for new plugins.

With these in place, begin migrating code in small PRs: transports first, then factory wiring, then UI.

### 0.1 Additive “Copy‑First” Migration (Recommended)

To avoid brittle file moves/deletions inside the existing plugin, use an additive strategy:

1) Create new top‑level plugin `Open3DBroadcast` with empty module skeletons for sender, receiver, shared, and transports.
2) Copy code from the old `Open3DStream` plugin into the new modules incrementally (common, non-transport-specific, and then transport‑by‑transport), preserving namespaces and public interfaces.
3) Disable registration in the old plugin (compile‑time define or config) while the new module takes over. Avoid two modules registering the same transport name.
4) Verify build + runtime for the new module in isolation; then delete the corresponding code from the old plugin in a subsequent PR when parity is proven.

Guards to prevent double registration:
- Primary: Project `.uproject` plugin list controls loading. Disable `Open3DStream` when testing new modules; enable only one at a time.
- Secondary (optional): compile‑time guard `O3D_ENABLE_LEGACY` if both must load concurrently for comparison.
- Log a single warning if both old and new attempt to register the same `TransportName` and hard‑fail the second registration in non‑shipping builds.

Temporary naming to reduce collisions while both exist:
- Keep canonical class names in the new modules (future‑proof).
- If necessary, temporarily suffix legacy implementations with `Legacy` only inside the old plugin to avoid ODR collisions while both are compiled.
- During the interim phase while the legacy plugin remains in the repository, the new shared module is introduced as `Open3DSharedNext` to avoid Unreal Build Tool `ModuleRules` naming collisions. Plan to rename it back to `Open3DShared` once the legacy module is retired.

Phased PR sequence (suggested):
1. PR A: Add new `Open3DBroadcast` plugin + empty module folders (`Open3DShared`, `Open3DReceiver`, `Open3DSender`, transports) + build flags + CI wiring. Update `ProjectSandbox.uproject` to enable `Open3DBroadcast` and disable `Open3DStream` by default (developers can re-enable the legacy plugin manually when they need parity validation).
2. PR B: Copy sockets transport (`Open3DTransportSockets`) + adapt factories; disable old sockets registration; tests green.
3. PR C: Copy NNG transport; disable old NNG registration; tests green.
4. PR D: Copy UDP sender/receiver (if distinct from sockets code) or integrate fully; introduce WebRTC backend abstraction + copy LiveKit implementation; disable old WebRTC registration; tests green.
5. PR E: Migrate remaining sender components to `Open3DSender` and receiver logic to `Open3DReceiver`; ensure factories use new module names; add URI canonicalization + config asset hooks. Update `ProjectSandbox.uproject` to disable `Open3DStream`.
6. PR F: Remove legacy broadcast/receiver code from old `Open3DStream` plugin; keep Core Redirects; Update docs/CHANGELOG; audit O3D_ENABLE_LEGACY=0 path.

### 0.2 Progress Logging Protocol

To maintain a deterministic view of migration progress, every coding agent MUST, immediately after completing any testable step (i.e., a change validated by a successful build, unit test, automation suite, or benchmark), record the current status in the **Progress Log** section of this document.

Each log entry MUST include:

- **Timestamp (UTC) & Step Name** — reference the relevant section/PR bullet (e.g., "2025-11-09 UTC – PR A: Shared helper rename stub").
- **Completed Work** — concise bullet list of the code/assets/tests that reached green-state in this step.
- **Verification** — explicit callout of the commands/tests/builds executed and their outcomes.
- **Open Questions / Risks** — note any blockers, new assumptions, or items requiring follow-up. Use `None` if clear.
- **Emergent / Follow-up Actions** — enumerate new work items generated by this step (including reminders to remove compatibility shims, schedule CI runs, etc.).

Additional rules:

1. Append the newest entry to the top of the log so the latest status is always first.
2. Keep entries brief (≤ six bullets total) to remain scannable.
3. If a testable step regresses previously completed work, include a pointer to the affected entry and describe remediation status.

## Progress Log

> Entries are listed in reverse chronological order (newest first).

### 2025-11-10 UTC – Sender/receiver refactor + build flag plumbing
- **Completed Work:** Finished decomposing `FO3DReceiverSource::HandleSerializedFrame` into parse/pose helpers, wrapped sender transport orchestration inside `FO3DSenderTransportController`, and introduced the shared `O3DModuleRules.ApplyTransportDefines` helper so every module (`Open3DSharedNext`, sender, receiver, loopback, sockets, NNG, WebRTC) respects the `O3D_*` build defines.
- **Verification:** `ProjectSandboxEditor Win64 Development` via `Build.bat` (UE 5.6 toolchain) now succeeds post‑refactor (`Result: Succeeded`, 12.4 s total).
- **Open Questions / Risks:** Need follow-up to reconcile `IOpen3DReceiver::Start` signature with the roadmap and to implement the forthcoming transport config asset / URI canonicalizer before enabling transport-specific automation.
- **Emergent / Follow-up Actions:** Implement `UO3DSTransportConfigAsset` + URI helpers, stage loopback round-trip automation (Step 5), and update docs once build flags are exercised in CI.

### 2025-11-10 UTC – Core module audit vs migration roadmap
- **Completed Work:** Reviewed Open3DSharedNext, Open3DSender, Open3DReceiver, and loopback transport implementations to map progress against Migration Steps 1–6; confirmed plugin gating and registry scaffolding align with Step 1/3 expectations while documenting remaining gaps.
- **Verification:** Inspection only (code review; no build or tests executed in this session).
- **Open Questions / Risks:** Receiver `Start` API still requires an injected consumer (deviates from spec); build flag defines from §4 are absent; transport config asset / canonical URI helpers not implemented; loopback lacks automated round-trip test coverage required for Step 6 sign-off.
- **Emergent / Follow-up Actions:** Add build flag plumbing across ModuleRules; reconcile `IOpen3DReceiver::Start` signature with design doc or update spec; implement `UO3DSTransportConfigAsset` and URI canonicalization pipeline; port sockets/NNG/WebRTC transports and author loopback round-trip automation in line with Steps 7–10.

> **Next Actions (2025-11-10):** Execute the design-pattern alignment plan — refactor oversized sender/receiver routines, introduce shared transport config assets + canonical URIs, wire `O3D_*` build flags through ModuleRules, and add loopback round-trip automation tests. Record each milestone above with verification details.

### 2025-11-09 UTC – Receiver transport-owned endpoint fields
- **Completed Work:** Removed generic receiver `Endpoint` / `StreamId` properties, added loopback customization widget to capture channel name, and moved URI assembly into the transport module so the Live Link panel no longer surfaces obsolete fields.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` (UE 5.6) succeeded after introducing the widget and transport config changes.
- **Open Questions / Risks:** Additional transports still need bespoke panels to expose their settings; WebRTC token UI remains pending.
- **Emergent / Follow-up Actions:** Implement WebRTC receiver customization, align sender workflow with the new pattern, and add automation coverage for transport-provided settings widgets.

### 2025-11-09 UTC – Receiver transport token modularization
- **Completed Work:** Removed the shared receiver `AuthToken` field, delegated credential handling to transport customizations, and hid the raw `TransportOptions` map from the generic Live Link factory details panel.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` (UE 5.6) succeeded after UHT regenerated headers with the updated config structure.
- **Open Questions / Risks:** WebRTC and other credentialed transports still need bespoke customization panels to surface their token inputs.
- **Emergent / Follow-up Actions:** Implement WebRTC receiver customization to own token UI, mirror the change on the sender path, and add regression coverage once transport-specific settings land.

### 2025-11-09 UTC – PR E: Receiver LiveLink factory wiring
- **Completed Work:** Added `UO3DReceiverSourceFactory` with Slate-based settings panel, updated `Open3DReceiver` module dependencies, and ensured receiver sources instantiate via serialized config.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` (UE 5.6) succeeded after introducing the factory (same session).
- **Open Questions / Risks:** Manual editor validation still pending to confirm Live Link menu entry behavior in practice.
- **Emergent / Follow-up Actions:** Schedule Live Link window sanity test, add automated coverage for receiver factory serialization once harness migrates.

### 2025-11-09 UTC – PR E: Receiver transport-specific UI gating
- **Completed Work:** Added conditional metadata so WebRTC-only receiver settings hide unless the WebRTC transport/backend is selected, preventing loopback and other transports from surfacing irrelevant fields.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` (UE 5.6) completed successfully after UHT regenerated headers.
- **Open Questions / Risks:** Longer-term architecture still needs transport modules to own their advanced settings; tracked as follow-up.
- **Emergent / Follow-up Actions:** Prototype transport-provided settings panels so WebRTC/NNG modules can contribute fields without core struct knowledge.

### 2025-11-09 UTC – PR B: Loopback transport implementation
- **Completed Work:** Implemented the loopback transport sender/receiver classes within `Open3DTransportLoopback`, added shared in-memory channel queues, and wired module startup to register the transport via the new sender/receiver registries.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` (UE 5.6) succeeded after introducing the loopback transport (same session).
- **Open Questions / Risks:** None at this stage; queue capacity defaults to 64 but is overridable via advanced params.
- **Emergent / Follow-up Actions:** Validate editor workflow using loopback transport UI, add automated loopback roundtrip test once test harness copy completes.

### 2025-11-09 UTC – Loopback cleanup & file split
- **Completed Work:** Split the loopback transport into dedicated `LoopbackChannel`, `LoopbackSender`, and `LoopbackReceiver` files, removed the legacy `LoopbackTransport` aggregate header/implementation, and updated module wiring to reference the new classes directly.
- **Verification:** Pending — run ProjectSandboxEditor Win64 Development build after refactor to confirm no regressions.
- **Open Questions / Risks:** None; removal of the aggregate files should not affect other modules due to registry-based factory registration.
- **Emergent / Follow-up Actions:** Add unit/automation coverage exercising loopback queue saturation once the receiver test harness is migrated.

### 2025-11-09 UTC – Sender/receiver registry split
- **Completed Work:** Split the shared transport registry into module-local `O3DSenderRegistry` and `O3DReceiverRegistry`, updated exports to use module APIs, and reinstated the shared umbrella header.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` succeeded after the refactor (UE 5.6 toolchain, same session).
- **Open Questions / Risks:** None observed; sender/receiver registries now operate independently with mirrored behaviour.
- **Emergent / Follow-up Actions:** Update transport modules to register via the new module-specific APIs and audit includes to drop legacy `O3DTransportRegistry` usage once consumers migrate.

### 2025-11-09 UTC – Transport interfaces & registry scaffolding
- **Completed Work:** Added shared transport interface definitions (`IOpen3DSender`, `IOpen3DReceiver`, `FO3DTransportConfig`, `FO3DTransportStats`) and implemented sender/receiver factory registries within `Open3DSharedNext`.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` succeeded after invalidating makefile (new sources compiled cleanly).
- **Open Questions / Risks:** None. Registry currently stores factories only; usage will be validated once transports migrate.
- **Emergent / Follow-up Actions:** Wire upcoming transport modules to register with the new registry and remove legacy factory code once parity is achieved.

### 2025-11-09 UTC – Shared helper renames + build verification
- **Completed Work:** Renamed Open3DSharedNext helper headers/sources (helpers, console vars, logs, loopback) from the `O3DS*` to `O3D*` prefix and introduced temporary compatibility shims where required.
- **Verification:** `ProjectSandboxEditor Win64 Development` build via `Build.bat` succeeded (no warnings beyond license reminder).
- **Open Questions / Risks:** None; compatibility macros will remain until downstream modules migrate.
- **Emergent / Follow-up Actions:** Track removal of compatibility aliases after dependent modules consume the new symbols.

## 1. Goals
1. Decouple transports so they can be enabled / disabled (compile‑time + run‑time) without cross‑contamination.
2. Allow building sender‑only or receiver‑only subsets to minimize footprint in projects that only broadcast or only consume.
3. Preserve all existing external behavior (LiveLink source discovery, settings UI, CLI tools, FlatBuffers payload format).
4. Provide a common interface layer for factories (`IOpen3DReceiver`, `IOpen3DSender`) so adding a new transport is additive and does not require touching existing transports.
5. Abstract WebRTC backends (LiveKit vs raw libdatachannel) behind a stable publisher / subscriber facade.
6. Enable targeted performance / reliability testing per transport (benchmarks & smoke tests) post‑refactor.

## 2. Scope
### In Scope
- Module splits for: Basic Sockets (TCP/UDP), NNG, WebRTC.
- Conditional compilation via new build flags (sender / receiver granularity + per‑transport toggles).
- Factory registration lifecycle adjustments.
- Shared configuration & URI scheme standardization.
- WebRTC backend abstraction interface.
- Test additions (unit + integration) & Definition of Done checklist.

### Out of Scope (Future Work)
- Introducing new protocol message types.
- Changing FlatBuffers schema (`o3ds.fbs`).
- Overhauling existing UI layouts beyond relocating controls if a module moves.
- Adding encryption layers (TLS/DTLS) beyond what already exists.

## 3. Proposed Module Layout (Single New Plugin)
```
Plugins/
	Open3DStream/                  (Legacy plugin; left untouched and disabled via ProjectSandbox.uproject during migration)

  Open3DBroadcast/               (NEW unified plugin hosting all modules)
    Source/
      Open3DShared/              (Shared utilities & protocol helpers)
			Open3DReceiver/            (Receiver orchestration, LiveLink integration)
			Open3DSender/              (Sender orchestration, component capture)
			Open3DTransportLoopback/   (In‑process loopback sender/receiver for validation & tests)
      Open3DTransportSockets/    (TCP + UDP sender/receiver + shared)
      Open3DTransportNNG/        (NNG sender/receiver + shared)
      Open3DTransportWebRTC/     (WebRTC sender/receiver + shared + backend abstraction)
```

Each transport module contains three logical areas (may be separated by folders or namespaces):
- `Shared/` (protocol adaptation, connection management primitives, common serialization helpers)
- `Sender/` (classes that implement `IOpen3DSender`)
- `Receiver/` (classes that implement `IOpen3DReceiver`)

Dependency boundaries (within `Open3DBroadcast` plugin):
- `Open3DTransportSockets` → depends on `Open3DShared`, optionally on `Open3DSender` (sender) and `Open3DReceiver` (receiver). NEVER sender ↔ receiver direct dependency.
- Same pattern for NNG & WebRTC modules.
- `Open3DTransportWebRTC` additionally depends on third‑party `libdatachannel` + any LiveKit client wrapper (under `thirdparty/libdatachannel`).
- Legacy plugin (`Open3DStream`) should not be referenced once migration complete; only kept for transitional Core Redirects.

## 4. Build Flags & Configuration Defines

Global build flags (added to all relevant modules’ `.Build.cs`):
| Flag | Meaning | Default |
|------|---------|---------|
| `O3D_BUILD_SENDER` | Compile sender code paths (broadcast) | `1` |
| `O3D_BUILD_RECEIVER` | Compile receiver code paths (ingest) | `1` |
| `O3D_WITH_TRANSPORT_SOCKETS` | Include TCP/UDP module | `1` |
| `O3D_WITH_TRANSPORT_NNG` | Include NNG module | `1` |
| `O3D_WITH_TRANSPORT_WEBRTC` | Include WebRTC module | `1` |
| `O3D_WEBRTC_BACKEND_LIVEKIT` | Enable LiveKit backend implementation | `1` |
| `O3D_WEBRTC_BACKEND_LIBDC` | Enable raw libdatachannel backend | `1` |
| `O3D_ENABLE_LEGACY` | Allow legacy transport registration (old plugin code) | `0` |

Selection rules:
- If `O3D_WITH_TRANSPORT_WEBRTC=0` both backend flags are ignored.
- At least one backend must be enabled if WebRTC module is built. If both are enabled a runtime config chooses primary.
- Legacy code paths only compiled / registered if `O3D_ENABLE_LEGACY=1`; new modules take precedence.

`.Build.cs` snippet example:
```csharp
bool bSender = true; // Could be read from environment or Target.cs
bool bReceiver = true;
PublicDefinitions.Add($"O3D_BUILD_SENDER={(bSender ? 1 : 0)}");
PublicDefinitions.Add($"O3D_BUILD_RECEIVER={(bReceiver ? 1 : 0)}");
PublicDefinitions.Add("O3D_WITH_TRANSPORT_SOCKETS=1");
PublicDefinitions.Add("O3D_WITH_TRANSPORT_NNG=1");
PublicDefinitions.Add("O3D_WITH_TRANSPORT_WEBRTC=1");
PublicDefinitions.Add("O3D_WEBRTC_BACKEND_LIVEKIT=1");
PublicDefinitions.Add("O3D_WEBRTC_BACKEND_LIBDC=1");
```

Environment overrides (developer convenience):
- `set O3D_BUILD_SENDER=0` (Windows) / `export O3D_BUILD_SENDER=0` (Unix) prior to invoking UAT will disable sender globally.

## 5. Interface Contracts

### Sender Interface (abstract)
```cpp
class IOpen3DSender
{
public:
		virtual ~IOpen3DSender() = default;
		virtual bool Initialize(const FO3DTransportConfig& Config) = 0; // Non-blocking; allocate resources.
		virtual bool Start() = 0;              // Begin async networking / connection establishment.
		virtual void Stop() = 0;               // Idempotent; safe to call multiple times.
		virtual bool Send(const O3DS::SubjectList& List) = 0; // Return false on backpressure / drop.
		virtual void Tick(float DeltaSeconds) = 0;            // Lightweight upkeep; NEVER block.
		virtual FO3DTransportStats GetStats() const = 0;
};
```

### Receiver Interface (abstract)
```cpp
class IOpen3DReceiver
{
public:
		virtual ~IOpen3DReceiver() = default;
		virtual bool Initialize(const FO3DTransportConfig& Config) = 0;
		virtual bool Start() = 0;
		virtual void Stop() = 0;
		// Poll should parse available packets and invoke a callback provided at creation
		// to publish subjects to LiveLink / application code.
		virtual int32 Poll() = 0; // returns number of SubjectLists processed.
		virtual FO3DTransportStats GetStats() const = 0;
};
```

### Common Types
- `FO3DTransportConfig`: Structured configuration (endpoint URI, credentials/token, backend preference, QoS settings).
- `FO3DTransportStats`: Counters (bytes sent/received, dropped frames, average latency, jitter window, reconnect attempts).

## 6. Factory Registration & Discovery

	- `RegisterReceiver(FName TransportName, TFunction<IOpen3DReceiver*()> CreateFn)`
	- `RegisterSender(FName TransportName, TFunction<IOpen3DSender*()> CreateFn)`
	- `Unregister*` counterparts.
 - Naming convention: "loopback", "sockets", "nng", "webrtc". Lowercase, stable.
- Runtime creation uses a URI scheme (see §7) to select a factory. Example: `webrtc://room/AvatarStream` resolves to WebRTC factory.

## 7. Configuration & Unified URI Scheme

To ensure consistent endpoint parsing across transports, adopt:

```
<scheme>://<host>[:port][/path][?key=value&key2=value2]
```

Schemes:
- `tcp://host:port` (Sender/Receiver)
- `udp://host:port` (Receiver focus; sender for discovery or broadcast only if already supported)
- `nng+pub://host:port` / `nng+sub://host:port`
- `webrtc://room/<RoomName>?backend=livekit|libdc&role=pub|sub`
- `livekit://room/<RoomName>?token=<JWT>&role=pub|sub` (alias; may be normalized internally to `webrtc://` with backend parameter)
 - `loopback://<ChannelName>?role=pub|sub` (in‑process pairing for validation; no host/port)

Configuration precedence:
1. Explicit URI query parameters.
2. Project settings (plugin config panel).
3. Environment variables (e.g. `O3D_WEBRTC_TOKEN`).

### 7.1 Independent UI Fields → Canonical URI / Config Asset

To preserve ease-of-use and existing UX, editors and tools will continue to expose discrete input fields:
- Transport (dropdown): `Sockets`, `NNG`, `WebRTC`
- Role (dropdown): `Sender` / `Receiver` (or `Publisher` / `Subscriber` for WebRTC)
- Backend (dropdown, WebRTC only): `LiveKit`, `libdatachannel`
- Host / Room / Stream ID (text)
- Port (numeric; hidden or disabled if not meaningful, e.g. LiveKit room)
- Token / Credentials (secret text; masked)
- Advanced (key/value list): latency target, max queue frames, TLS toggle, ICE servers list, etc.

At creation time these are canonicalized into either:
1. A URI string (see §7) e.g. `webrtc://room/AvatarStream?backend=livekit&role=pub&token=<JWT>`
2. Or a `UO3DSTransportConfigAsset` (new Unreal data asset type) that stores structured fields and can generate a URI on demand.

#### Canonicalization Rules
- Empty optional fields are omitted from query string.
- Token never persisted to source-controlled asset unless explicitly allowed (flag `bPersistToken` in asset); default false.
- Backend omitted if only one compiled backend available.
- Role translated to `role=pub|sub` for WebRTC; for sockets/NNG role may be implicit (sender vs receiver factory chosen directly) and not encoded in URI.
 - Loopback always encodes role (`loopback://Channel?role=pub` or `role=sub`) to ensure deterministic pairing; ChannelName is the StreamId.

#### Unreal Data Asset
```cpp
UCLASS(BlueprintType)
class UO3DSTransportConfigAsset : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Transport; // "sockets" | "nng" | "webrtc"

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Role; // "sender" | "receiver" | "pub" | "sub"

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Backend; // webRTC only: "livekit" | "libdc"

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString HostOrRoom;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Port = 0; // 0 if unused

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString StreamId; // Optional stream/channel identifier

	// For loopback transport, StreamId doubles as the ChannelName.

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FString Token; // Optional; not serialized if bPersistToken == false

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bPersistToken = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TMap<FString, FString> AdvancedParams; // Additional key/value pairs

	UFUNCTION(BlueprintCallable)
	FString ToCanonicalURI() const; // Implements rules above
};
```

`ToCanonicalURI()` constructs the scheme based on `Transport` then appends host/room, stream id, and query params. If `bPersistToken` is false, runtime assembly can pull token from environment variable `O3D_WEBRTC_TOKEN` or a secure storage provider. The factory creation functions accept either a raw URI or a config asset reference; asset variant internally calls `ToCanonicalURI()`.

#### Factory Creation Overloads
```cpp
IOpen3DSender* Open3DSenderFactory::CreateFromURI(const FString& CanonicalURI);
IOpen3DSender* Open3DSenderFactory::CreateFromConfig(const UO3DSTransportConfigAsset* ConfigAsset);
IOpen3DReceiver* Open3DReceiverFactory::CreateFromURI(const FString& CanonicalURI);
IOpen3DReceiver* Open3DReceiverFactory::CreateFromConfig(const UO3DSTransportConfigAsset* ConfigAsset);
// Settings UI support
TSubclassOf<class UO3DTransportSettingsBase> Open3DTransportRegistry::GetSettingsClass(FName TransportName);
UO3DTransportSettingsBase* Open3DTransportRegistry::CreateDefaultSettings(FName TransportName, UObject* Outer);
```

Both paths converge after URI parsing. This keeps testing simple (URI path) while allowing richer editor workflows (asset path).

#### UX Considerations
- Existing UI panels simply populate fields; a preview box shows the generated URI (read-only) with a copy button.
- Validation occurs per field (e.g., port range, token presence if backend demands auth) before enabling “Connect” / “Start Streaming”.
- AdvancedParams surfaced in a collapsible section to avoid clutter.

#### 7.1.1 Pluggable Per‑Transport Settings (no central code changes)
- Define an abstract settings base that transports extend:
```cpp
UCLASS(Abstract, BlueprintType, EditInlineNew)
class UO3DTransportSettingsBase : public UObject
{
	GENERATED_BODY()
public:
	// Convert settings object to query string parameters (urlencoded where needed)
	UFUNCTION(BlueprintNativeEvent)
	FString ToQueryString() const;

	// Optional validation hook used by generic UI
	UFUNCTION(BlueprintNativeEvent)
	bool Validate(FString& OutError) const;
};
```
- Each transport module declares a derived settings UObject (e.g., `UO3DWebRTCSettings`, `UO3DSocketsSettings`, `UO3DLoopbackSettings`) with UPROPERTY fields. No edits to shared code required.
- Transport registers its settings type and factory with `Open3DTransportRegistry` at `StartupModule()`.
- Central UI hosts a generic `IDetailsView` bound to the created settings UObject for the selected transport. The preview URI is assembled as:
  `scheme://host[:port]/path?` + `TransportSettings->ToQueryString()` + `&` + serialized AdvancedParams.

- `UO3DSTransportConfigAsset` holds an instanced settings object:
```cpp
UCLASS(BlueprintType)
class UO3DSTransportConfigAsset : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Instanced, BlueprintReadWrite)
	UO3DTransportSettingsBase* TransportSettings; // Created via registry per transport
	// ... existing fields ...
};
```

This enables adding a new transport (e.g., QUIC/WebTransport) by adding a module that registers its factory + settings type; the generic UI updates automatically without modifying shared code.

#### Serialization & Privacy
- Tokens not stored by default; if `bPersistToken=false` the asset writer zeroes the string during save.
- Build system / CI should not leak tokens—tests use placeholder tokens or environment overrides.
- Assets may be shared between team members without exposing credentials.

This approach satisfies ease-of-use while keeping the internal routing unified and deterministic.

## 8. WebRTC Backend Abstraction

Introduce interface:
```cpp
class IWebRTCBackend
{
public:
		virtual ~IWebRTCBackend() = default;
		virtual bool Initialize(const FO3DTransportConfig& Config) = 0;
		virtual TSharedPtr<IWebRTCPublisher> CreatePublisher(const FString& StreamId) = 0;
		virtual TSharedPtr<IWebRTCSubscriber> CreateSubscriber(const FString& StreamId) = 0;
		virtual void Tick(float DeltaSeconds) = 0; // Housekeeping (ICE timeout checks, stats gather)
		virtual void Shutdown() = 0;
};
```
Backends provide consistent publisher/subscriber objects exposing data channel send/receive and stats queries. LiveKit backend wraps existing signaling; libdatachannel backend performs direct peer negotiation.

### 8.1 Loopback Transport Design Considerations
- Purpose: rapid in‑process validation of sender/receiver paths without network variability.
- Implementation: shared ring buffer or lock‑free queue pair keyed by ChannelName + role.
- Sender path (`role=pub`): writes serialized `SubjectList` payloads to queue; receiver path (`role=sub`) polls queue during `Poll()`.
- Backpressure: if queue is full, sender drops frame (increments `DroppedFrames`) identical to other transports.
- Stats: latency measured as time since enqueue timestamp; injects negligible transport latency (<1ms) to serve as baseline.
- Isolation: No special casing in `Open3DSender` / `Open3DReceiver`; they interact purely via factory + interface.
- Testing: Unit tests use loopback first for deterministic frame round‑trip before enabling network transports.

Runtime selection:
- Config key: `webrtc.backend` or URI `backend=` query.
- Fallback order: LiveKit → libdatachannel if primary initialization fails and secondary available.

## 9. Threading & Performance Constraints
- No blocking on game thread (Start/Initialize must spawn async tasks or schedule work).
- Network callbacks marshal payload decoding to worker threads, then game thread only for LiveLink publishing.
- Backpressure: `Send()` returns false immediately if output queue is full; queue size limit configurable (`transport.maxQueueFrames`).
	- Sockets/NNG/WebRTC all adopt identical drop reporting counter `DroppedFrames`.
- Stats collection performed incrementally in `Tick()` to avoid spikes.

## 10. Testing & Validation Plan
| Test Type | Purpose | Location |
|-----------|---------|----------|
| Unit (Factory Registration) | Ensure modules register/unregister cleanly | New tests under `test/` |
| Unit (URI Parsing) | Validate scheme routing & parameter extraction | `test_tx_seq_tests.cpp` extension |
| Unit (Loopback Roundtrip) | Validate sender→receiver frame integrity in-process | New `TestLoopbackTransport.cpp` |
| Integration (Sockets) | Loopback sender→receiver pose roundtrip | New `TestSocketsTransport.cpp` |
| Integration (NNG) | Pub/sub multi-frame sequence reliability | New `TestNNGTransport.cpp` |
| Integration (WebRTC) | Publisher/subscriber handshake & frame flow | New `TestWebRTCTransport.cpp` |
| Performance Bench | Throughput & latency baseline vs pre-refactor | Reuse existing bench harness (add per-transport) |
| Smoke (LiveKit) | Two-editor live session, measure jitter/drop | Script `smoke-webrtc` |

Automation gating: CI must run unit + integration tests with all transports compiled; nightly may run performance benches.

## 11. Migration Steps
**Follow this exact order unless otherwise instructed**
1. Create new module folders & minimal `.uplugin` / `.Build.cs` stubs.
2. Migrate CORE (non‑transport) code first to establish test harness:
	- Move/rename sender component classes (`Open3DBroadcastComponent`, `O3DSBroadcastComponent`, etc.) into `Open3DSender` as `O3DSenderComponent` (final name: `UO3DSenderComponent`).
	- Move/rename LiveLink source / receiver classes (`Open3DStreamSource`, etc.) into `Open3DReceiver` prefixed `O3DReceiver*` (e.g., `FO3DReceiverSource`).
	- Move protocol/model helpers (e.g., `UnrealModel`, skeletal conversion utilities) into `Open3DShared` with minimal renaming unless they carry old plugin prefixes.
	- Establish factory scaffolding with stub transports (temporarily only a dummy no‑op transport) so receiver/sender tests can compile.
3. Introduce interfaces + factories adjustments (compile with all flags enabled first).
4. Add build flags & conditional compilation scaffolding.
5. Implement URI parsing & routing utilities (loopback entries accepted but not active yet).
6. Add `Open3DTransportLoopback` module (now testable because core sender/receiver exists) and implement loopback transport; run unit round‑trip tests.
7. Migrate sockets transport code; register factories; add integration test.
8. Migrate NNG transport code; register factories; add integration test.
9. Introduce WebRTC backend abstraction; migrate LiveKit implementation; register factories; add integration test.
10. Expand tests (URI parsing, per‑transport settings objects) & performance benchmarks; compare (≤5% variance allowed unless justified).
11. Update README(s) and CHANGELOG (module split + build flags section).
 12. Add new CI workflows targeting only the `Open3DBroadcast` plugin modules (skip legacy plugin build where possible).
 11. Remove any Core Redirects added for migration only once downstream projects updated (only if names changed).
 12. Gate old vs new plugin via `ProjectSandbox.uproject` plugin entries; do not modify legacy plugin contents.

## 12. Risks & Mitigations
| Risk | Impact | Mitigation |
|------|--------|-----------|
| Hidden cross-dependency (sender using receiver util) | Compilation failures / circular includes | Enforce folder segregation & include audits in PR review |
| Runtime ordering (factories used before registration) | Null pointers / crashes | Late-binding via `GetSenderFactory()` that asserts non-null; module load order documented |
| Performance regression due to extra indirection | Increased latency | Benchmark early, inline lightweight factory lookups |
| WebRTC backend mismatch in config | Connection failures | Fallback mechanism + clear log warning |
| Flag misconfiguration | Missing functionality unexpectedly | Emit startup summary log of active transports |

## 13. Performance & Telemetry
## 13.1 CI / GitHub Actions Strategy

Goals:
- Provide incremental builds focusing on new modules (`Open3DBroadcast`) to shorten feedback loop.
- Preserve a full regression build (including legacy `Open3DStream`) nightly until migration complete.
- Surface transport-specific unit test results (loopback, sockets, nng, webrtc) as separate job outputs.

Workflows:
1. `ci-plugin-open3dbroadcast.yml` (PR-triggered)
	 - Triggers: `pull_request` on branch `feature/transport-modules`.
	 - Jobs:
		 - Setup cache (Unreal Intermediate + DerivedDataCache if feasible).
		 - Configure build to disable legacy plugin via `.uproject` edit (ensure `Open3DStream` is disabled).
		 - Build editor target with modules: `Open3DShared`, `Open3DReceiver`, `Open3DSender`, `Open3DTransportLoopback` (later expand to sockets, nng, webrtc as they migrate).
		 - Run loopback unit tests only (fast path) initially; expand test matrix as transports migrate.
		 - Upload artifacts: build logs, test XML, stats JSON.
2. `ci-full-regression.yml` (scheduled nightly)
	 - Triggers: `schedule` (cron) + manual dispatch.
	 - Builds all plugins (legacy + new) with all flags enabled.
	 - Runs full unit + integration suite (sockets, nng, webrtc smoke, loopback).
	 - Benchmarks (optional step) behind input flag.
3. `ci-benchmark-transports.yml` (manual dispatch)
	 - Focus: performance benchmarking across transports; publishes Markdown summary to PR comment when invoked.

Sample minimal PR workflow snippet (`.github/workflows/ci-plugin-open3dbroadcast.yml`):
```yaml
name: Open3DBroadcast Plugin CI
on:
	pull_request:
		branches: [ feature/transport-modules ]

jobs:
	build-and-test:
		runs-on: windows-latest
		steps:
			- name: Checkout
				uses: actions/checkout@v4

			- name: Setup Python
				uses: actions/setup-python@v5
				with:
					python-version: '3.11'

			- name: Cache Unreal Derived Data (optional)
				uses: actions/cache@v4
				with:
					path: ProjectSandbox/DerivedDataCache
					key: ddc-${{ runner.os }}-${{ hashFiles('ProjectSandbox/ProjectSandbox.uproject') }}
					restore-keys: |
						ddc-${{ runner.os }}-

			- name: Disable legacy plugin in uproject
				run: |
					python scripts/update_uproject.py --disable Open3DStream --enable Open3DBroadcast ProjectSandbox/ProjectSandbox.uproject

			- name: Build Editor (Open3DBroadcast subset)
				run: |
					& "C:\Program Files\Epic Games\UE_5.6\Engine\Build\BatchFiles\Build.bat" ProjectSandboxEditor Win64 Development .\ProjectSandbox\ProjectSandbox.uproject -waitmutex

			- name: Run Loopback Unit Tests
				run: |
					pwsh -File Build/Scripts/Run-AutomationTests.ps1 -Filter Loopback

			- name: Upload Logs
				uses: actions/upload-artifact@v4
				with:
					name: broadcast-ci-logs
					path: Build/Logs
```

Implementation Notes:
- `scripts/update_uproject.py` would perform JSON edit (toggle plugin entries) without relying on manual diffs.
- Test filtering uses an explicit tag or naming convention (e.g. `Loopback` in test names).
- As transports migrate, workflow matrix expands: `strategy.matrix.transport: [loopback, sockets, nng, webrtc]` to run targeted subsets concurrently.
- Add a final summary step to parse test results and annotate PR (GitHub summary markdown).

Security & Caching:
- Ensure no secrets (tokens) needed for loopback/sockets/NNG tests; WebRTC tests may require ephemeral token server mock skipped in fast CI.
- Use separate cache keys to avoid polluting baseline with experimental builds.

Removal Plan:
- After legacy plugin disable milestone (DoD ticked), consolidate nightly workflow to drop legacy build.

- Unified stats aggregated per transport and exposed via existing debug UI (unchanged user workflow).
- Add per-transport stat categories: `Sockets`, `NNG`, `WebRTC`.
- Log only state transitions (Connected, Disconnected, BackendFallback, QueueDropWarning).

## 14. Definition of Done Checklist
- [ ] All three transport modules compile independently with sender-only, receiver-only, both.
- [ ] Existing external APIs (LiveLink subject naming, component properties) unchanged.
- [ ] All unit + integration tests pass; new tests added.
- [ ] Performance benchmarks within tolerance.
- [ ] WebRTC can swap backends via config without restart (if both compiled).
- [ ] No blocking calls on game thread (audited).
- [ ] README & CHANGELOG updated (module layout + flags).
- [ ] Startup log lists active transports and backends.
 - [ ] Config asset to URI canonicalization implemented & preview UI present.
 - [ ] Tokens excluded from serialized assets unless explicitly allowed.
 - [ ] Factory overloads (`CreateFromConfig`) covered by tests (round-trip URI equivalence).
 - [ ] Legacy plugin disabled via `.uproject` (no legacy module duplication required).
 - [ ] Core Redirects audited; only permanent redirects retained.
 - [ ] Sandbox `.uproject` lists only `Open3DBroadcast` (legacy `Open3DStream` removed/disabled).
 - [ ] Loopback transport registered and passes roundtrip tests (baseline latency logged).
 - [ ] Generic UI renders per-transport settings via registered UObject without modifying shared code.
 - [ ] New GitHub Actions workflow(s) added:
	 - [ ] PR CI for Open3DBroadcast-only subset (loopback tests at minimum) is green.
	 - [ ] Nightly full regression includes both legacy and new plugin until cutover.
	 - [ ] Artifacts uploaded (logs/test results), and path filters skip building legacy when only new plugin changes.

## 15. Example Startup Log (Target State)
```
[Open3DStream] Active transports: sockets(sender,receiver), nng(receiver), webrtc(sender,receiver; backend=livekit | fallback=libdc)
[Open3DStream] Build flags: SENDER=1 RECEIVER=1 WEBRTC=1 SOCKETS=1 NNG=1
```

## 16. Open Questions (Clarification Needed)
1. Exact naming for new transport module folders (`Open3DTransport*` vs `Open3D*Transport`).
	- Answer:  Prefer `Open3DTransport*`.
2. Whether UDP sender functionality is required or receiver-only (broadcast semantics today?).
	- Answer: Both are required (sender based on current `O3DSUdpTransport` class).
3. Location for URI parsing helpers (central utility module vs each transport?).
	- Answer: With each transport module to keep new transports self-contained.
4. LiveKit token acquisition workflow (environment variable only vs UI field?).
	- Answer: Token server on the LiveKit machine; token generation hidden from user.
5. Minimum stats granularity desired in UI (per-frame vs averaged window).
	- Answer: Averaged window.
6. Confirm acceptance of additive copy-first migration plan and defaulting `O3D_ENABLE_LEGACY=0`.
	- Answer: Accepted. Additionally, confirm single new plugin `Open3DBroadcast` will replace old `Open3DStream` plugin (legacy disabled by default during migration).

If unresolved, these should be confirmed before implementation; defaults in this doc are provisional.

---
### Summary
This refactor isolates transport logic into modular, flag-driven build units while preserving external behaviors. Factories + URI routing provide extensibility; WebRTC backend abstraction future-proofs signaling. Tests & performance benchmarks guard against regressions. Proceed once open questions are clarified or accepted with defaults.

---
Whenever there is ambiguity, request clarification (see §16) before making irreversible implementation decisions.

---
## Appendix A: Class Renaming & Mapping Guidelines

Purpose: Ensure deterministic naming and avoid lingering legacy prefixes during migration.

| Legacy Name / Pattern | New Name / Pattern | Notes |
|-----------------------|--------------------|-------|
| `Open3DBroadcastComponent`, `O3DSBroadcastComponent` | `O3DSenderComponent` / `UO3DSenderComponent` | Prefix with `O3DSender` to indicate module ownership and drop legacy `Broadcast` suffix. Unreal class keeps `U` prefix. |
| `Open3DStreamSource` (LiveLink) | `O3DReceiverLiveLinkSource` / `FOpen3DReceiverSource` | Use `O3DReceiver` prefix; keep existing LiveLink integration traits. |
| `Open3DStream*` (other receiver helpers) | `O3DReceiver*` | Systematic replace `Open3DStream` → `O3DReceiver` where class signifies receiver concern. |
| `Open3DBroadcast*` (other sender helpers) | `O3DSender*` | Systematic replace `Open3DBroadcast` → `O3DSender`. |
| `UnrealModel` | (Optionally) `O3DSharedUnrealModel` or retain `UnrealModel` if widely referenced | Rename only if conflict risk; prefer stability. |
| Transport classes `TcpConnector`, `UdpConnector`, `NNGConnector`, `WebRTCConnector` | `O3DTransportSocketsTcpConnector`, etc. OR keep short names within their module namespace | Avoid overlong names; clarity via module folder and namespace might suffice. |

Namespace Strategy:
- Keep existing `O3DS::` protocol namespace for data structures.
- For Unreal module code, prefer top-level naming without adding deep nested namespaces; rely on module segmentation for clarity.

Refactoring Rules:
1. Rename only after the file physically moves into its destination module to simplify diffs.
2. Do NOT alter public method signatures during the initial move; rename first, then extend in separate PR if needed.
3. Add transitional `using OldName = NewName;` aliases ONLY if downstream code outside this repo consumes headers; remove before final DoD completion.
4. Update includes and forward declarations to new paths; avoid wildcard includes.
5. Run a comprehensive search for legacy prefixes to ensure no leftovers (`grep -R "Open3DBroadcast"`, `grep -R "Open3DStream"`).

Testing Sequence Alignment:
- After step 2 (core migration), implement a minimal smoke test using a dummy transport stub to assert that `O3DSenderBroadcastComponent` generates frames and `O3DReceiverLiveLinkSource` ingests them (no network).
- After step 6 (loopback), replace dummy stub with loopback transport; all prior smoke tests should pass unmodified—verifies transport interchangeability.

Documentation Updates:
- In each PR description, include a short mapping table for renamed classes touched in that PR only.
- Changelog entry: "Refactor: Renamed legacy classes to O3DSender*/O3DReceiver* patterns (no behavioral changes)."

Definition of Done Addendum:
- All legacy class name prefixes removed from active code paths (except intentional protocol/API names).
- Search for `Open3DStream` / `Open3DBroadcast` yields only legacy plugin references or historical docs.
- Loopback smoke test uses identical sender/receiver code paths as other transports (no `#ifdef` special cases).