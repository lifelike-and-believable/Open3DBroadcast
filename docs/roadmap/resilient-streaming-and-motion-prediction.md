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
5. **LiveLink division of labor (RESOLVED — default posture).** The UE receiver is a LiveLink source, and LiveLink already buffers per subject and does presentation-time sampling with interpolation and a configurable evaluation offset (`BufferSettings` / interpolation processors). We therefore split responsibilities rather than duplicate them:
   - **O3DB owns the network layer (pre-LiveLink):** the A1 reorder/dedup/stale-drop window, loss detection, and — critically — **mapping the sender's `tx_wallclock_us` into the local engine-time domain** so each LiveLink frame gets a correct `WorldTime`/`SceneTime`. O3DB hands LiveLink a clean, correctly-timestamped, monotonic frame stream and does **not** build its own interpolation/jitter buffer.
   - **LiveLink owns presentation:** time-based interpolation and the latency-vs-smoothness offset. Configure a sane default subject buffer offset that covers expected jitter; expose it.
   - **Concealment (C1) is the one exception where O3DB synthesizes frames:** only when a gap/lateness exceeds what LiveLink interpolation covers gracefully does O3DB push a *predicted* LiveLink frame (corrected when the real frame lands). **Latency-hiding (rendering ahead of the newest received frame) is a separate opt-in mode, default OFF**, because it changes the latency/accuracy tradeoff and interacts with LiveLink's offset.
   - **The real sub-problem this creates: clock-offset estimation.** To map `tx_wallclock_us` → local time you need an estimate of `(local_recv - tx_wallclock)`; use a min-filter / robust moving estimate of that difference (its minimum over a window ≈ one-way delay + fixed skew). Fall back to receive-time timestamping when `tx_wallclock_us` is unset (legacy) or the offset can't yet be estimated. This estimator is an explicit A2 sub-task.
   - **Verify against the target UE version:** exact `BufferSettings` fields, the frame-time API on `FLiveLinkFrameDataStruct` / how `WorldTime` and `SceneTime` are set, and default interpolation behavior. Do not hardcode assumptions — confirm in the LiveLink source before building A2.
6. **Quaternion handling.** Rotation prediction/extrapolation must operate in a valid space (slerp / exp-map on the unit quaternion, then renormalize); never lerp raw components. Curves and translations are linear and simpler.

---

## 3. Workstream A — Reliability & Sequencing

Goal: the stream degrades gracefully on jitter, reorder, and loss, and its behavior is measurable.

### Phase A1 — Sequencing & transmit clock (core)

**Goal:** every frame carries a real monotonic `tx_seq` and transmit wall-clock; a receiver-side gate orders, dedups, and drops stale frames, emitting loss/reorder stats. Entirely in `src/o3ds` (engine-agnostic, Linux-CTest-able). **Dependencies:** none. **Out of scope:** time-based jitter buffering and concealment (A2/C1).

This phase has three cohesive pieces; an agent can land them as a small stack (schema → sender → gate).

#### A1.a — Schema & wire (append-only)
- Add to the **end** of the `SubjectList` table in `src/o3ds.fbs` (never reorder/remove existing fields; FlatBuffers assigns field IDs by declaration order, so appending is the compatible move):
  ```
  tx_seq:ulong = 0;          // monotonic per logical stream; 0 = unset
  tx_wallclock_us:ulong = 0; // UTC microseconds at transmit; 0 = unset
  ```
- Regenerate `src/o3ds_generated.h` with `flatc --cpp src/o3ds.fbs` (checked in; never hand-edit). **Mirror both the schema and the generated header into the vendored copy under `ProjectSandbox/.../ThirdParty/open3dstream/` until #204 lands** (§0.2).
- **Do not conflate with the existing `time:double` field** — that is the animation/sample time (content clock). `tx_wallclock_us` is a *new, separate* value: when the frame left the sender. Both coexist.
- Compatibility: an old sender never sets these → new receiver reads default `0` → treated as unset → gate bypassed (legacy path). An old receiver ignores the new fields. Verify both directions in tests.

#### A1.b — Sender: sequence source + clock (`src/o3ds/sequencing.*`)
- Provide `class SequenceCounter { std::atomic<uint64_t> mNext{1}; uint64_t Next(){ return mNext.fetch_add(1, std::memory_order_relaxed); } }` and `uint64_t NowUtcMicros()` (`std::chrono::system_clock` → µs since epoch).
- **`tx_seq` is per *logical stream*, not per process.** One counter per sender→receiver stream so the receiver sees a contiguous `1,2,3,…` and a gap unambiguously means loss. A process-global counter would make fan-out to N receivers look like N× loss — avoid it. The counter's natural home is the per-stream serializer on the sender (`O3DSenderSerializer`), which passes `Next()` and `NowUtcMicros()` in per frame.
- Extend the core serialize entry points **by appending defaulted params** (keeps every existing caller source-compatible):
  `int Serialize(std::vector<char>& out, double time, uint64_t tx_seq = 0, uint64_t tx_wallclock_us = 0);` (same for `SerializeUpdate`). Write them into the flatbuffer; `0` means "unset" by convention.
- `tx_wallclock_us` uses UTC (`system_clock`) because it must be comparable *across machines*; document that it can step under NTP adjustment and is therefore for latency/staleness only — **ordering is driven solely by `tx_seq`, which is strictly monotonic.**

#### A1.c — Receiver: reorder / dedup / stale gate (`src/o3ds/reorder_gate.*`)
A per-source state machine. Operate on a small value `Frame { uint64_t seq; double wallclock_s; std::vector<char> bytes; }`. The receiver verifies the buffer **once** at ingress (reuse the hardened `Verifier` path), reads `tx_seq`/`tx_wallclock_us` from the verified root, constructs a `Frame`, and calls `Push`; parse-and-apply happens lazily **on delivery** (so buffered-then-dropped frames are never fully parsed). Suggested shape:
```cpp
struct ReorderStats { uint64_t delivered=0, dup_dropped=0, stale_dropped=0, lost=0, reordered=0; };

class ReorderGate {
public:
    struct Config { uint32_t max_window = 16; double max_delay_s = 0.05; int64_t reset_backjump = 256; };
    explicit ReorderGate(Config = {});
    // emit is called, in ascending seq order, for each frame ready to apply.
    void Push(Frame&& f, double now_s, const std::function<void(Frame&&)>& emit);
    void Flush(double now_s, const std::function<void(Frame&&)>& emit); // timeout / teardown
    const ReorderStats& Stats() const;
};
```
Behavior for incoming seq `S` (with `last_applied` = highest delivered seq):
- **`S == 0` (unset/legacy):** bypass entirely — emit immediately in arrival order (preserves today's behavior; supports old senders and mixed streams).
- **First frame:** initialize `last_applied = S - 1` so `S` delivers immediately.
- **`S <= last_applied`:** duplicate or late-past-successor → **drop** (`dup_dropped` if seen before, else `stale_dropped`); never un-apply.
- **`S == last_applied + 1`:** emit; then drain any buffered consecutive successors (`+2, +3, …`), advancing `last_applied` (each drained out-of-order arrival counts `reordered`).
- **`S > last_applied + 1`:** gap → buffer `S`. Recover if the missing seq(s) arrive within `max_window` frames / `max_delay_s`. If the window fills or the timeout elapses (checked on `Push`/`Flush`), **give up the gap**: emit buffered frames in order, advance `last_applied` past the hole, count skipped seqs as `lost`.
- **Publisher restart:** the counter resets to `1`, so `S` jumps far below `last_applied`. Detect a backward jump `> reset_backjump` (or ≥K consecutive sub-`last_applied` frames) and **re-baseline** to the new session (else the stream would drop forever). *This is the case the optional `frame_epoch` field (§2.1) makes unambiguous — recommend deciding here whether to add `frame_epoch:uint = 0` now rather than rely on the heuristic.*

#### A1 — Acceptance (core, CTest-wired; extend the ASan/UBSan harness)
- In-order run → all delivered, `lost==0`.
- **Reorder recovered:** arrive `100, 102, 101` → emit `100,101,102`, `reordered==1`, `lost==0`.
- **Stale-after-successor:** arrive `100, 101, 99` → emit `100,101`, `99` dropped stale, `lost==0`.
- **Duplicate:** `100, 100` → second `dup_dropped`.
- **Gap timeout:** `100, 102`, advance clock past `max_delay_s` with no `101` → emit `102`, `lost==1`.
- **Window overflow:** persistent gap fills window → flush past hole, correct `lost`.
- **Legacy:** `tx_seq==0` frames pass straight through in arrival order.
- **Restart:** seq jumps backward beyond `reset_backjump` → re-baseline, delivery resumes.
- **Safety:** malformed/short buffers and unverifiable roots at ingress → treated as unset/dropped, never crash (ASan/UBSan clean).

#### A1 — Open decisions (resolve in the design step)
- Add `frame_epoch` now (clean restart handling) vs. rely on the backward-jump heuristic? (Leaning: add it — it's cheap and removes ambiguity.)
- Default `max_window` / `max_delay_s` (starting point 16 frames / 50 ms ≈ 3 frames @ 60 fps); expose as config/console vars in A2.
- Payload storage in the gate: `std::vector<char>` bytes (simple, one copy) vs. type-erased handle (zero-copy, more complex). Bytes are fine to start.

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
- **Goal:** an append-only, streamable, truncation-tolerant capture container and a core library reader/writer.
- **Files/modules:** new `src/o3ds/capture.*` (engine-agnostic).
- **`.o3dscap` byte-level spec (v1, all multi-byte fields little-endian):**

  **Header** (variable length, `header_len` bytes total):

  | Offset | Size | Field | Notes |
  |---|---|---|---|
  | 0 | 8 | `magic` | ASCII `O3DSCAP\0` (`4F 33 44 53 43 41 50 00`) |
  | 8 | 2 | `format_version` | `uint16` = 1 |
  | 10 | 2 | `header_len` | `uint16`, total header size incl. variable tail |
  | 12 | 4 | `flags` | `uint32`; bit0 = timestamps are UTC-µs; other bits reserved (0) |
  | 16 | 8 | `base_wallclock_us` | `uint64`, capture start (0 = unset) |
  | 24 | 4 | `schema_fingerprint` | `uint32`, e.g. CRC32 of `o3ds.fbs` or version-tag hash |
  | 28 | 4 | `predictor_version` | `uint32` (0 = none) |
  | 32 | 2 | `source_desc_len` | `uint16` |
  | 34 | `source_desc_len` | `source_desc` | UTF-8 free-form (transport, host, notes) |
  | … | pad | — | header zero-padded to `header_len` |

  **Records** (repeat until EOF):

  | Offset | Size | Field | Notes |
  |---|---|---|---|
  | 0 | 8 | `recv_wallclock_us` | `uint64`, capture/receive time (absolute) |
  | 8 | 4 | `wire_len` | `uint32` |
  | 12 | `wire_len` | `wire_bytes` | the exact on-the-wire O3DS frame, verbatim |

- **Reader rules:** validate `magic`/`format_version` (reject unknown major); loop records; if `< 12` bytes remain, or `wire_len` exceeds remaining file bytes → treat as a truncated final record and **stop cleanly, not error** (captures may be cut mid-write). **Reject `wire_len > 64 MB`** to avoid huge allocations from a corrupt file (mirror the parser-hardening posture). No per-record checksum by default; reserve a `flags` bit for optional CRC32 later.
- **Writer:** appends header once, then one record per frame; capture is verbatim wire bytes → near-zero cost, safe to run inline.
- **Acceptance:** core round-trip unit test (write N frames → read back identical bytes + timestamps); truncated/garbage/oversized-`wire_len` files handled without crashing (feed to the ASan harness). CTest-wired.
- **Dependencies:** ideally A1 (so captures carry seq/clock in the wire bytes), but the container itself is agnostic and tolerates unset. **Out of scope:** UI, network injection (B2).

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
- **Files/modules:** new `src/o3ds/predict/` (`pose_predictor.h`, baseline impls).
- **Interface (starting point — an agent may refine names/signatures, keep the shape):**

  ```cpp
  namespace O3DS {

  // A flat, topology-stable snapshot of one subject's animatable values.
  // Channel counts/order are fixed for a subject "epoch" (until a keyframe
  // changes topology); this keeps the predictor pure-math and decoupled from
  // the FlatBuffers/Subject types so it unit-tests without the model layer.
  struct PoseSample {
      double   t   = 0.0;   // sample time, seconds, in the SENDER clock domain
      uint64_t seq = 0;     // tx_seq of the source frame (0 = unset)
      std::vector<Vec3>  translations;  // per node
      std::vector<Quat>  rotations;     // per node (unit quaternion)
      std::vector<Vec3>  scales;        // per node
      std::vector<float> curves;        // per curve
  };

  class IPosePredictor {
  public:
      virtual ~IPosePredictor() = default;
      virtual uint32_t Version() const = 0;            // carried in-stream for residual mode
      virtual void Observe(const PoseSample& sample) = 0;   // feed confirmed frames in order
      virtual bool Predict(double t, PoseSample& out) const = 0; // false => insufficient history (caller holds last)
      virtual void Reset() = 0;                        // keyframe / topology change / version mismatch / large gap
  };

  } // namespace O3DS
  ```

- **Baseline implementations:**
  - `HoldPredictor` (**Version 0**): `Predict` returns the last observed sample. Exactly today's behavior; the universal fallback.
  - `LinearPredictor` (**Version 1**): constant velocity. Linear channels: `x(t) = x1 + (x1−x0)/(t1−t0)·(t−t1)`. Rotations: angular velocity from `dq = q1 · q0⁻¹` → axis-angle → scale by `(t−t1)/(t1−t0)` → apply to `q1`, renormalize (§2.6). Guard tiny/zero `dt`.
  - `QuadraticPredictor` (**Version 2**): constant acceleration (needs 3 samples); more responsive but overshoots — bound the horizon.
- **Approach:** bounded history (ring of the last few samples); allocation-light (runs per-frame); the caller owns the `PoseSample ↔ O3DS::Subject` mapping (a small adapter), keeping the predictor free of engine/model types.
- **Acceptance:** core unit tests on synthetic motion — `LinearPredictor` error ≪ `HoldPredictor` at horizon = 1 frame; quaternion predictions stay unit-norm; short/degenerate history returns `false` cleanly. CTest-wired. No UE dependency.
- **Dependencies:** A1 (needs seq/timing). **Out of scope:** wiring into send/receive paths (C1/C2).

- **Named hook points (for C1/C2 — documented here so C0's shape is right):**
  - **Receiver concealment (C1):** the receiver keeps one predictor per subject, calls `Observe()` on every applied frame; when the A2 gate reports a gap at presentation time it calls `Predict(t_now)` and pushes that as the LiveLink frame; on the real frame's arrival, `Observe()` it and optionally blend-correct.
  - **Sender residual (C2):** in `SubjectList::SerializeUpdate`, replace the implicit "compare to last-sent value" with "compare to `predictor.Predict(t)`", encode the residual under the existing `deltaThreshold`, then `predictor.Observe(current)`. The `HoldPredictor` makes this reduce **exactly** to today's delta scheme — so C2 is a strict generalization, and shipping C0+`HoldPredictor` changes nothing observable (good for landing the plumbing safely).

### Phase C1 — Receiver-side concealment & latency-hiding (UE glue)
- **Goal:** when a frame is missing/late, synthesize a predicted pose instead of freezing or popping; optionally render slightly ahead to hide RTT. **Receiver-only, no determinism requirement** → first real payoff.
- **Files/modules:** `Open3DReceiver/O3DReceiverSource.cpp`; `O3DPerformanceMetrics` (prediction-error metric: compare predicted vs the real frame when it later arrives).
- **Approach:** drive the A2 gate; on a gap, feed `IPosePredictor.Predict()` to the LiveLink apply path (respecting §2.5). Bound the extrapolation horizon; always correct toward the next real frame (avoid visible snap — consider a short blend).
- **Acceptance:** replay (B2) with injected loss shows measurably reduced visible "pops" and a reported prediction-error metric; with zero loss, behavior is identical to today (predictor is a no-op when every frame arrives on time). Manual + replay-driven.
- **Dependencies:** C0, A2, B2. **Out of scope:** changing what the sender transmits.

### Phase C2 — Symmetric residual compression (core + both paths)
- **Goal:** sender transmits `actual − predicted` residuals; receiver reconstructs. Bandwidth win + graceful degradation. **Requires the two ends to predict from the same history.**
- **⚠️ The load-bearing constraint — history divergence under loss.** Residual coding assumes sender and receiver derive the *same* prediction. That holds only if they share the same observed history. But the predictor's history is prior *poses*, and on a **lossy/unordered transport the receiver never sees the frames it dropped** → its history diverges from the sender's → every subsequent residual decodes wrong until the next keyframe. This is a correctness problem, not just a quality one, and it's distinct from float determinism. Two ways out:
  - **(a) Scope C2 to reliable/ordered transports only** (TCP, WebRTC reliable data channel, MoQ reliable streams). No loss → no divergence → the common case works with a strong (chained) predictor. **Recommended default.** On unreliable transports (UDP, MoQ datagrams), C2 is disabled and those links use **keyframe + independent deltas + concealment (C1)** instead.
  - **(b) Keyframe-relative, independently-decodable P-frames:** predict each P-frame **only from the last keyframe + its own offset**, never from prior P-frames, so a lost P-frame doesn't corrupt its neighbors. Loss-tolerant but weaker prediction (shorter effective history) → smaller compression win. Viable fallback for unreliable transports if (a)'s "concealment-only" isn't enough.
  - A back-channel/ACK scheme (predict from last-*acked*) is explicitly **out of scope** — not all transports are bidirectional.
- **Files/modules:** `src/o3ds/model.cpp` (`SerializeUpdate`/`ParseUpdate` — generalize the delta path into residual coding), `src/o3ds/predict/`, schema (`predictor_version`, keyframe/epoch marker per §2), both UE paths, transport capability flag (reliable vs unreliable).
- **Approach:** both ends run the same versioned predictor; sender quantizes+thresholds the residual (small residual → send nothing — today's delta, but relative to a smart prediction); periodic keyframes bound drift, enable join-in-progress, and re-anchor after loss. Gate the whole path on the transport's reliability capability per the constraint above. On `predictor_version` mismatch or unreliable transport, fall back to today's last-pose delta.
- **Acceptance:** on a reliable transport + representative corpus, bytes/frame drop materially vs today's delta scheme at equal visual error; interop fallback with an old peer verified; on an unreliable transport, C2 stays disabled and C1 concealment carries the loss case. Core round-trip unit tests + replay.
- **Dependencies:** C0, A1; §2.3–2.4 determinism decisions; transport reliability capability flag. **Out of scope:** learned model, back-channel ACKs.
- **Risks:** (1) history divergence under loss — handled by the scoping above; (2) float determinism across platforms/compilers — mitigate with keyframe cadence, tolerance-based reconstruction, or a fixed-point predictor; **prototype and measure drift on the target platforms before committing to (a) with a chained predictor.**

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
- **History divergence under loss (C2)** — residual coding silently breaks on lossy transports because the receiver's pose history diverges from the sender's; scope C2 to reliable/ordered transports (or use keyframe-relative independent deltas), and let C1 concealment carry unreliable links. Load-bearing — see C2.
- **Float determinism** for residual coding (C2) — measure drift before committing; keyframe cadence / fixed-point as mitigations.
- **LiveLink division of labor** — RESOLVED (§2.5): LiveLink owns presentation smoothing; O3DB owns the network layer + clock-offset→timestamp mapping + optional predicted-frame concealment. Verify the exact LiveLink buffer/time APIs against the target UE version before A2.
- **Clock-offset estimation** (§2.5, A2) — mapping `tx_wallclock_us` to local engine time needs a robust moving/min estimate of the send→recv offset; absolute one-way latency additionally needs NTP-level sync, else report relative jitter/loss only.
- **Inference budget** for the learned model (C3) — hard real-time constraint; keep C3 a gated spike.
- **Two-copy schema edits** until #204 lands.

## 9. Definition of done (program level)

- `tx_seq`/`tx_wallclock_us` shipped, populated, and consumed; legacy interop preserved.
- Receiver reorders/dedups/drops-stale and reports latency/jitter/loss on the HUD.
- Capture/replay format + CLI exist; replay drives deterministic tests.
- A pluggable predictor with classical baselines conceals loss/hides latency on the receiver, measured against real frames.
- (Stretch) residual compression reduces bytes/frame at equal visual error; (research) learned predictor beats baselines within budget.
- New core logic is unit-tested and running in CI.
