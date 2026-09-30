# Open3DTransportMoQ — Phase 3 Readiness Brief

_Last updated: 2025-11-24_

## 1. Purpose
Provide the coding agent with a concise, actionable overview of what must be completed before starting Phase 3 (receiver implementation) of the MoQ transport plan.

## 2. Status Snapshot

| Phase | Complete | Outstanding items |
| --- | --- | --- |
| **0 – Third-Party Integration** | ✅ Build.cs links Win64 artifacts, runtime loader (`MoQFfiSupport`) validates DLL, `O3D_WITH_TRANSPORT_MOQ.md` explains flag. | ⚠️ `ThirdParty/moq-ffi/README.md` is missing (no vendored commit/hash, no refresh workflow). Linux/Mac placeholders absent; non-Windows builds fail immediately. Error messages/docs still point to the missing README. |
| **1 – Unreal Wrappers** | ✅ RAII handles (`MoQHandles.*`), session wrapper (`MoQSessionWrapper.*`), async dispatcher (`MoQAsyncDispatcher.*`), and error helpers (`MoQTypes.*`) match design. Positive-path automation now exists in `MoQSessionWrapperTests` to verify connection/subscriber callbacks dispatch on the game thread. | — |
| **2 – Sender** | ✅ `FO3DMoQSender` implements Initialize/Start/Stop, queueing, worker thread, reconnection logic; module compiles cleanly (`buildoutput_moq.txt` 11/22). | ⚠️ `GetStats()` lacks moq-transport metrics (task 2.9). Cloudflare relay tests now run end-to-end but still depend on public infrastructure—need a local relay/automation target plus telemetry. Receiver remains a stub. |

## 3. Required Actions Before Phase 3

### 3.1 Phase 0 Gaps
1. **Vendored dependency manifest (tasks 0.1 & 0.6).**
   - Add `ThirdParty/moq-ffi/README.md` with upstream repo + commit SHA, artifact hashes, refresh steps (summarize `ProjectSandbox/External/moq-ffi/README.md`).
   - Update all references (`Open3DTransportMoQ.Build.cs`, `MoQFfiSupport.cpp`, `O3D_WITH_TRANSPORT_MOQ.md`, `CLOUDFLARE_RELAY_TESTING.md`) so they point to the new README.
2. **Cross-platform placeholders (task 0.2).**
   - Provide `Linux/Mac` stub folders under `ThirdParty/moq-ffi/lib|bin` or gate the module so MoQ auto-disables on unsupported platforms.
   - Document current support status in `O3D_WITH_TRANSPORT_MOQ.md`.

### 3.2 Phase 1 Gap
✅ **Completed:** `MoQSessionWrapperTests` now include dispatcher-driven connection and subscriber callbacks, proving the wrapper surfaces positive-path behavior on the game thread without relying on the live relay. No further Phase 1 blockers remain.

### 3.3 Phase 2 Gaps
4. **Cloudflare/local relay automation (tasks 2.4–2.8).**
   - Latent refactor + documentation updates are complete; next step is wiring the suite into CI with a deterministic local relay (`O3D_MOQ_RELAY_URL`) so results are reliable.
   - Capture sample logs/artifacts for troubleshooting, and set expectations for runtime (~2 minutes) in test descriptions.
5. **Expose relay metrics (task 2.9).**
   - Surface moq-transport stats through `FO3DMoQSender::GetStats()` (e.g., relay RTT, objects in flight). If FFI lacks APIs, add TODOs with a tracking issue.
6. **Improve diagnostics.**
   - Add targeted `UE_LOG` statements around publisher lifecycle and payload publish failures so receiver work has observability.

## 4. Risks & Test Gaps
- **Dependency provenance**: Without a vendored README + checksums, future refreshes may silently drift.
- **Platform instability**: Linux/Mac builds break immediately when MoQ is enabled; Phase 3 needs deterministic behavior across platforms.
- **Wrapper coverage gap**: `MoQSessionWrapperTests` still lack a positive-path subscriber/publisher test that exercises `FMoQAsyncDispatcher` without relying on the external relay.
- **Relay availability**: Even with latent commands, the Cloudflare-backed suite can still flap when the public relay is offline or congested—CI needs a local relay to stay deterministic.
- **Stats blind spots**: No relay-level telemetry makes diagnosing subscription failures difficult.

## 5. Recommended Sequence
1. **Housekeeping**: Add the vendored README, fix references, and address platform gating/placeholders.
2. **Testing infrastructure**: Add the missing wrapper-level positive test and wire the Cloudflare relay suite into CI using a local relay target.
3. **Diagnostics**: Expose moq-transport metrics and tighten logging.
4. **Proceed to Phase 3** only after the above items are merged and validated.

---
Once these prerequisites are met, the coding agent can confidently start implementing `FO3DMoQReceiver` per the Phase 3 task list (Initialize/Start/Subscribe/Poll, resubscription, latency stats, etc.).