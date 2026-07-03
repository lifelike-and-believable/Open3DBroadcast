# Plan: Resilient Streaming & Motion Prediction

**Status:** Draft for delegation · **Owner:** TBD · **Audience:** coding agents + maintainers

This document is a delegable implementation plan for three interlocking workstreams:

- **A — Reliability & Sequencing:** make the stream survive real (WAN) networks.
- **B — Record & Replay:** capture/replay the wire protocol deterministically.
- **C — Predictive Motion:** a shared pose predictor for loss concealment, latency-hiding, and residual compression.

It is written so an agent can pick up any **phase**, produce its own task breakdown, and implement it against clear acceptance criteria. Read §0–§2 first regardless of which phase you take.

---

## 0. How to use this document (for coding agents)

- Each workstream is split into **phases**. Each phase has: **Goal · Files/modules · Approach · Acceptance criteria · Dependencies · Out of scope**. Treat a phase as one PR (or a small stack), not one commit.
- Before writing code for a phase, produce a short task breakdown and confirm the **Open design questions** in that section have answers (some are deliberately left open — resolve them in the design step, don't guess silently).
- Follow the repo's existing rules in `.github/copilot-instructions.md` and `AGENTS.md`: no blocking the game thread; capture happens after animation evaluation; verify Unreal API signatures against the engine version the plugin targets; `src/o3ds.fbs` is the authoritative schema and `src/o3ds_generated.h` is regenerated with `flatc --cpp`, never hand-edited.
- **Schema changes are append-only.** Never reorder or remove existing FlatBuffers fields; only add new fields at the end of a table with defaults, so old and new peers stay compatible.
- **Don't claim done until verified.** State explicitly what was built vs tested.

### 0.1 Core-first principle (important)

Put as much logic as possible in the **engine-agnostic core library (`src/o3ds/`)**, not in the Unreal plugin. The core builds and unit-tests with plain CMake/g++ on Linux, which is the only place CI can actually run tests today (the UE build is on a self-hosted Windows runner). Sequencing math, the capture/replay format, and the predictor core all belong in `src/o3ds` behind small interfaces; the UE plugin should be thin glue (LiveLink apply, HUD, capture UI). This maximizes what the CI we can run actually verifies.

### 0.2 Prerequisite hazard — core library duplication

The core library is currently checked in **twice**: `src/o3ds/` and a verbatim copy under `ProjectSandbox/Plugins/Open3DBroadcast/ThirdParty/open3dstream/include/o3ds/` (tracked as issue #204). **Until that duplication is resolved, every serialization/schema change in this plan must be applied to both copies, or the plugin will silently use a stale core.** Strongly prefer resolving #204 (single source of truth, consumed via submodule or a packaging step) as an early task so the rest of this plan touches one copy. If #204 is not yet done, each phase that edits core files must note "mirror into the vendored copy" in its task list.

---

## 1. North star & architecture recap

Open3DBroadcast streams the **final evaluated pose** (skeletal transforms + facial/morph curves) plus audio between Unreal instances (and other consumers). Because it transports the *final* pose:

- Input-side concerns (capture-source variety, retargeting of the driver, ARKit ingest) are **upstream, handled by LiveLink/Unreal** and out of scope here.
- Output-side retargeting to the display rig is **downstream, handled by Unreal's runtime retargeting** and out of scope here.

O3DB's job is therefore to be the **best possible low-latency, reliable pipe for evaluated animation.** This plan is about the pipe.

### 1.1 The unifying idea

The existing delta serializer (`SubjectList::SerializeUpdate(..., deltaThreshold, ...)` in `src/o3ds/model.cpp`) is already a **predictor** with the crudest possible model: *predict each value stays equal to last frame; send only what changed beyond a threshold.* Workstreams A and C are what happens when that idea is taken seriously:

- Replace "predict = last pose" with a real **predictor run identically on both ends**.
- Sender transmits the **residual** (actual − predicted) → compression.
- On loss/lateness, receiver falls back to its own **prediction** → concealment / latency-hiding.

This maps directly onto the protocol that already exists: a full `Subject` descriptor is an **I-frame (keyframe)**; a `SubjectUpdate` is a **P-frame (predicted)**. We are making the "P-frame" prediction smart and measurable.

### 1.2 Key existing anchors

- Schema: `src/o3ds.fbs` → `SubjectList { subjects:[Subject]; updates:[SubjectUpdate]; time:double; }`.
- Serialize/parse: `src/o3ds/model.cpp` — `Serialize` / `SerializeUpdate` / `Parse` / `ParseUpdate`.
- UE send path: `Open3DSender/` (`O3DSenderComponent`, `O3DSenderSerializer`).
- UE receive path: `Open3DReceiver/O3DReceiverSource.cpp` (`HandleSerializedFrame`, `BuildSubjectPose`) — a LiveLink source that pushes static/frame data into `ILiveLinkClient`.
- Transports: `Open3DTransport{Sockets,NNG,WebRTC,MoQ,Loopback}`, behind `IOpen3DSender` / `IOpen3DReceiver` and the `ISerializedFrameConsumer` registry (`Open3DShared`).
- Telemetry: `O3DPerformanceMetrics` (`Open3DShared`) + its `o3d.ProfileGuide` HUD.
- Relay: `apps/Repeater/` (engine-agnostic, C++).

---

## 2. Cross-cutting design decisions

Resolve these once; every workstream depends on them.

1. **Frame identity & clock.** Add to the `SubjectList` table (append-only):
   - `tx_seq:ulong = 0;` — monotonic per-publisher sequence (0 = unset).
   - `tx_wallclock_us:ulong = 0;` — UTC microseconds at transmit (0 = unset).
   - (Design-to-confirm) `frame_epoch:uint = 0;` and/or explicit `is_keyframe:bool` if prediction needs an unambiguous I-frame marker beyond "has `subjects`". Decide in A1/C0.
2. **Backward compatibility.** Unset (0) fields must behave exactly like today's stream. A new receiver talking to an old sender (no seq/clock) must fall back cleanly; an old receiver ignores the new fields.
3. **Predictor determinism tiers.**
   - *Concealment / latency-hiding* is **receiver-only** and needs no cross-peer determinism → lowest risk, do first.
   - *Residual compression* requires **bit-compatible prediction on both ends** → needs a versioned, pinned predictor and periodic keyframes to bound drift → higher risk, do later.
4. **Predictor versioning.** Carry a `predictor_version` (negotiated at session start or stamped on keyframes) so sender/receiver agree on the model. Mismatch → fall back to keyframe/last-pose behavior.
5. **LiveLink interaction (open question, decide in A2/C1).** The UE receiver is a LiveLink source and LiveLink already has its own timestamped buffering/interpolation. Decide the division of labor: O3DB should own **network** concerns (dedup, reorder, loss detection, concealment decisions) and hand clean, correctly-timestamped frames to LiveLink, letting LiveLink do presentation smoothing — *unless* we want latency-hiding beyond LiveLink's, in which case O3DB synthesizes predicted frames and pushes them. Do not build a second buffer that fights LiveLink's; measure first.
6. **Quaternion handling.** Rotation prediction/extrapolation must operate in a valid space (slerp / exp-map on the unit quaternion, then renormalize); never lerp raw components. Curves and translations are linear and simpler.

---

## 3. Workstream A — Reliability & Sequencing

Goal: the stream degrades gracefully on jitter, reorder, and loss, and its behavior is measurable.

### Phase A1 — Sequencing & transmit clock (core)
- **Goal:** every frame carries a real monotonic `tx_seq` and `tx_wallclock_us`; receivers can order, dedup, and drop stale frames.
- **Files/modules:** `src/o3ds.fbs` (+ regenerate `src/o3ds_generated.h`); `src/o3ds/model.cpp` (`Serialize`/`SerializeUpdate` populate the fields; `Parse` reads them); a small `src/o3ds/sequencing.*` helper (per-publisher atomic counter + UTC-us clock). Mirror into the vendored core copy if #204 is unresolved.
- **Approach:** monotonic per-process counter starting at 1; `tx_wallclock_us` from a UTC clock. Receiver-side: a small reorder/dedup gate keyed on `tx_seq` (accept in-order, buffer small out-of-order window, drop duplicates and frames older than the last-applied seq).
- **Acceptance:** unit tests in the core (Linux/CTest) covering: increasing seq, out-of-order `[100,101,99,102]` reorders correctly, duplicate `101` dropped, stale frame after a newer one dropped, and unset (0) fields → legacy passthrough. Wire the tests into CTest so CI runs them (ties to the "CI verifies nothing" gap).
- **Dependencies:** none. **Out of scope:** jitter buffering by time, concealment.

### Phase A2 — Jitter/latency handling on the receiver (UE glue)
- **Goal:** absorb network jitter and expose end-to-end latency/loss without fighting LiveLink.
- **Files/modules:** `Open3DReceiver/O3DReceiverSource.cpp` (apply path), `O3DPerformanceMetrics`.
- **Approach:** resolve design decision §2.5 first. Feed the A1 reorder/dedup gate; compute end-to-end latency from `tx_wallclock_us` vs receive time (document clock-skew caveats — this is one-way delay, not RTT, and depends on NTP-level sync; treat absolute value as indicative, trends as reliable). Surface latency, jitter, loss %, reorder count in the existing HUD.
- **Acceptance:** with a recorded session replayed under injected jitter/loss (Workstream B), the HUD reports plausible latency/loss and the applied pose stream is monotonic (no backward jumps). Manual + replay-driven verification.
- **Dependencies:** A1; benefits from B2. **Out of scope:** prediction/concealment (Workstream C).

### Open questions (A)
- One-way latency needs sender/receiver clock sync to be absolute; do we require/assume NTP, or report only relative jitter + loss? Decide in A2.
- Reorder window size / max buffering delay — expose as a console var with a sane default.

---

## 4. Workstream B — Record & Replay

Goal: capture a live wire stream to disk and replay it deterministically. Pulls double duty as **network-test harness** and **training-data pipeline** for Workstream C.

### Phase B1 — Capture format + core reader/writer
- **Goal:** an append-only, streamable capture container and a core library reader/writer.
- **Files/modules:** new `src/o3ds/capture.*` (engine-agnostic). Proposed `.o3dscap` layout: a header (magic, format version, schema/`predictor_version`, source description, base clock) followed by records `[recv_wallclock_us][wire_len:uint32][wire_bytes]`. Wire bytes are exactly the on-the-wire frame (already length-delimited FlatBuffers), so capture is near-zero-cost.
- **Approach:** writer appends records; reader iterates records with their timestamps. Keep it format-versioned and forward-compatible.
- **Acceptance:** core round-trip unit test (write N frames → read back identical bytes + timestamps); malformed/truncated file handled without crashing (feed it to the ASan harness). CTest-wired.
- **Dependencies:** ideally A1 (so captures carry seq/clock), but format must tolerate unset. **Out of scope:** UI, network injection.

### Phase B2 — Replay engine + network-condition injector
- **Goal:** replay a capture through any transport (or directly into a receiver) at real or accelerated speed, with optional loss/jitter/reorder injection.
- **Files/modules:** `src/o3ds` replay driver; a small CLI app under `apps/` (mirrors `apps/Repeater`); optional hook so the UE receiver can consume a replay as a virtual transport.
- **Approach:** read records, re-emit respecting inter-arrival gaps (or as-fast-as-possible for deterministic tests); pluggable "channel model" that drops/delays/reorders per configurable probabilities with a fixed seed for reproducibility.
- **Acceptance:** replaying a capture with zero loss reproduces the original applied poses bit-for-bit; with seeded 5%-loss/120ms-jitter the output is deterministic across runs. Becomes a reusable fixture for A2 and C1 tests.
- **Dependencies:** B1. **Out of scope:** learned-model training loop (that consumes B output but lives in Workstream C tooling).

### Phase B3 — Capture at the repeater (optional, high-value)
- **Goal:** record sessions network-side with no UE in the loop (field debugging, corpus building).
- **Files/modules:** `apps/Repeater/` — add an opt-in capture sink writing `.o3dscap`.
- **Acceptance:** repeater relays and simultaneously writes a valid capture; smoke-tested in the Docker repeater image.
- **Dependencies:** B1.

---

## 5. Workstream C — Predictive Motion

Goal: a pluggable pose predictor that delivers concealment, latency-hiding, and (later) residual compression. **Staged, baseline-first** — prove the whole harness with classical math before any ML.

### Phase C0 — Predictor interface + classical baselines (core)
- **Goal:** a clean `IPosePredictor` abstraction with non-ML implementations, unit-tested in the core.
- **Files/modules:** new `src/o3ds/predict/` — `IPosePredictor` (per-subject state; `Update(pose, seq, t)`; `Predict(horizon) -> Pose`); implementations `HoldPredictor` (== current behavior), `LinearPredictor` (constant velocity), `QuadraticPredictor` (constant acceleration). Quaternion extrapolation per §2.6.
- **Approach:** operate on the existing pose representation (translations, quaternions, scale, curves). Keep it allocation-light and fast (this will run per-frame).
- **Acceptance:** core unit tests: on synthetic smooth motion, `LinearPredictor` error ≪ `HoldPredictor` error at horizon=1; quaternion predictions stay unit-norm; degenerate/short history handled. CTest-wired. No UE dependency.
- **Dependencies:** A1 (needs seq/timing). **Out of scope:** wiring into send/receive paths.

### Phase C1 — Receiver-side concealment & latency-hiding (UE glue)
- **Goal:** when a frame is missing/late, synthesize a predicted pose instead of freezing or popping; optionally render slightly ahead to hide RTT. **Receiver-only, no determinism requirement** → first real payoff.
- **Files/modules:** `Open3DReceiver/O3DReceiverSource.cpp`; `O3DPerformanceMetrics` (prediction-error metric: compare predicted vs the real frame when it later arrives).
- **Approach:** drive the A2 gate; on a gap, feed `IPosePredictor.Predict()` to the LiveLink apply path (respecting §2.5). Bound the extrapolation horizon; always correct toward the next real frame (avoid visible snap — consider a short blend).
- **Acceptance:** replay (B2) with injected loss shows measurably reduced visible "pops" and a reported prediction-error metric; with zero loss, behavior is identical to today (predictor is a no-op when every frame arrives on time). Manual + replay-driven.
- **Dependencies:** C0, A2, B2. **Out of scope:** changing what the sender transmits.

### Phase C2 — Symmetric residual compression (core + both paths)
- **Goal:** sender transmits `actual − predicted` residuals; receiver reconstructs. Bandwidth win + inherent graceful degradation. **Requires bit-compatible prediction on both ends.**
- **Files/modules:** `src/o3ds/model.cpp` (`SerializeUpdate`/`ParseUpdate` — generalize the delta path into residual coding), `src/o3ds/predict/`, schema (`predictor_version`, keyframe marker per §2), both UE paths.
- **Approach:** both ends run the same versioned predictor over confirmed history; sender quantizes+thresholds the residual (small residual → send nothing, like today's delta but relative to a smart prediction); periodic keyframes bound drift and enable join-in-progress/error recovery. On `predictor_version` mismatch, fall back to today's last-pose delta.
- **Acceptance:** on a representative corpus, bytes/frame drop materially vs the current delta scheme at equal visual error; a dropped P-frame is concealed by prediction with bounded error until the next keyframe; interop fallback with an old peer verified. Core round-trip unit tests + replay.
- **Dependencies:** C0, A1; determinism decisions §2.3–2.4. **Out of scope:** learned model.
- **Risk:** float determinism across platforms/compilers. Mitigations: keyframe cadence to bound drift, tolerance-based reconstruction, or fixed-point predictor. Prototype/measure drift before committing.

### Phase C3 — Learned predictor (research → productization)
- **Goal:** swap a small learned motion model in behind `IPosePredictor` for better prediction than the classical baselines.
- **Files/modules:** `src/o3ds/predict/LearnedPredictor.*` + an offline training pipeline (separate tooling dir) consuming `.o3dscap` corpora from Workstream B.
- **Approach:** start from the C1/C2 harness (interface, metrics, corpus all already exist). Favor small, fast models (compact GRU/MLP or similar) that meet a strict per-frame inference budget; ship weights as versioned assets keyed to `predictor_version`. Begin with concealment (C1, no determinism needed) before attempting learned residual coding (C2 determinism constraints).
- **Acceptance:** learned predictor beats the best classical baseline on held-out corpus prediction error while meeting the inference-time budget; falls back safely on version mismatch or budget overrun.
- **Dependencies:** C0–C2, B (corpus + replay eval). **Out of scope:** anything requiring determinism until the C2 float-determinism question is settled.
- **Note:** this is the most speculative phase. Treat C3 as a **spike with a go/no-go** gated on the prediction-error numbers from C1/C2, not a committed deliverable.

---

## 6. Sequencing & dependency graph

```
A1 (seq/clock, core)  ──┬──► A2 (jitter/latency, UE)
                        │
                        ├──► C0 (predictor iface + baselines, core) ──► C1 (concealment, UE)
                        │                                               │
B1 (capture format) ──► B2 (replay + injector) ──────────────► (fixtures for A2, C1)
        │                                                              │
        └──► B3 (repeater capture, opt)                                ▼
                                                        C2 (residual compression)  ──► C3 (learned)
```

Recommended order: **A1 → B1 → B2 → A2 → C0 → C1** (this delivers resilience + concealment + the test harness), then evaluate **C2**, then a gated **C3** spike. A1, B1, and C0 are independent enough to run in parallel by separate agents.

---

## 7. Testing strategy

- **Core-first (§0.1):** sequencing (A1), capture round-trip (B1), predictor math (C0), residual round-trip (C2) are all pure C++ — unit-test in the core and **wire into CTest so the Linux CI actually runs them.** Reuse the ASan/UBSan harness pattern already used for the parser-hardening work; feed malformed inputs to every new parser/reader.
- **Replay-as-test (B2):** the primary integration harness. Golden `.o3dscap` sessions + seeded channel models give deterministic, UE-free (or UE-lite) regression tests for A2 and C1.
- **UE automation:** extend the existing automation suites for the receiver apply-path changes; run them on the self-hosted runner (note: gated on the PR being out of draft).
- Every phase's acceptance criteria above names its concrete test.

---

## 8. Risks & open questions (consolidated)

- **Core duplication (#204)** — resolve early or every core change is doubled (§0.2).
- **Float determinism** for residual coding (C2) — measure drift before committing; keyframe cadence / fixed-point as mitigations.
- **LiveLink buffering interaction** (§2.5) — don't build a competing buffer; measure and decide the division of labor in A2/C1.
- **Clock sync** — absolute one-way latency needs NTP-level sync; may report only relative jitter/loss (A2).
- **Inference budget** for the learned model (C3) — hard real-time constraint; keep C3 a gated spike.
- **Two-copy schema edits** until #204 lands.

## 9. Definition of done (program level)

- `tx_seq`/`tx_wallclock_us` shipped, populated, and consumed; legacy interop preserved.
- Receiver reorders/dedups/drops-stale and reports latency/jitter/loss on the HUD.
- Capture/replay format + CLI exist; replay drives deterministic tests.
- A pluggable predictor with classical baselines conceals loss/hides latency on the receiver, measured against real frames.
- (Stretch) residual compression reduces bytes/frame at equal visual error; (research) learned predictor beats baselines within budget.
- New core logic is unit-tested and running in CI.
