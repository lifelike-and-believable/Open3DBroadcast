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

The core library is currently checked in **twice**: `src/o3ds/` and a verbatim copy under `ProjectSandbox/Plugins/Open3DBroadcast/ThirdParty/open3dstream/include/o3ds/` (tracked as issue #203). **Until that duplication is resolved, every serialization/schema change in this plan must be applied to both copies, or the plugin will silently use a stale core.** Strongly prefer resolving #203 (single source of truth, consumed via submodule or a packaging step) as an early task so the rest of this plan touches one copy. If #203 is not yet done, each phase that edits core files must note "mirror into the vendored copy" in its task list.

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
   - `tx_seq:ulong = 0;` — monotonic per **logical stream** (sender→receiver), not per process, so a gap means loss rather than fan-out (see A1.b). 0 = unset.
   - `tx_wallclock_us:ulong = 0;` — UTC microseconds at transmit (0 = unset); distinct from the existing `time:double` content clock.
   - (Design-to-confirm, leaning yes) `frame_epoch:uint = 0;` — bumped per publisher session so the receiver detects a sender restart unambiguously instead of via a backward-jump heuristic (A1.c). Decide in A1. A separate `is_keyframe` is likely unnecessary — "has `subjects`" already marks an I-frame — but confirm in C2.
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
- Regenerate `src/o3ds_generated.h` with `flatc --cpp src/o3ds.fbs` (checked in; never hand-edit). **Mirror both the schema and the generated header into the vendored copy under `ProjectSandbox/.../ThirdParty/open3dstream/` until #203 lands** (§0.2).
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

### Phase A2 — Receiver integration: gate wiring, clock mapping, metrics

**Goal:** wire A1's gate into the UE receiver, map the sender clock onto LiveLink frame time (per the resolved §2.5 posture — LiveLink owns presentation smoothing; O3DB owns the network layer + clock mapping), and surface latency/jitter/loss on the HUD. **Dependencies:** A1; tested via B2. **Out of scope:** prediction/concealment (Workstream C). Follow **core-first (§0.1)** — the estimator math goes in core (Linux-testable); UE files are thin glue.

#### A2.a — Wire `ReorderGate` into the receiver apply path (UE glue)
- **Files:** `Open3DReceiver/O3DReceiverSource.cpp` (`HandleSerializedFrame` ingress + the poll/tick).
- Ingress: verify the buffer once, extract `tx_seq`/`tx_wallclock_us`, build a `Frame`, `gate.Push(...)`. The gate's `emit` callback does the existing parse+apply (`BuildSubjectPose` → LiveLink push) — so reorder/dedup/stale-drop happens **before** the expensive apply, and superseded frames are never parsed.
- Call `gate.Flush(now)` from the receiver's existing tick/poll so gap-wait timeouts release buffered frames even when no new frame arrives (not only on `Push`).
- **Threading:** the gate is **not** thread-safe — confine it to one thread and match the receiver's existing worker-thread-ingest / game-thread-apply split; do not introduce a new cross-thread path. (LiveLink push itself is callable off the game thread, but keep to the existing pattern.) One gate per source; if a source ever multiplexes multiple senders, key gates by sender identity (note the caveat, don't build it yet).

#### A2.b — Clock-offset estimator (core: `src/o3ds/clock_offset.*`)
The real sub-problem behind §2.5. Pure math → **put it in core and unit-test it on Linux.**
- For each frame, `offset_i = local_recv_i − tx_wallclock_i = skew + delay_i`, where `delay_i ≥ 0`. The **minimum** `offset` over a window ≈ `skew + min_delay` (the least-delayed frame is closest to pure clock skew). Maintain a **rolling minimum** (min-filter over a bounded time/count window) as the offset estimate — the classic NTP-style approach.
- **Mapped presentation time (local) = `tx_wallclock + rolling_min_offset`.** A frame's `offset − rolling_min` is its *excess delay* → feeds the jitter metric.
- Handle clock steps (NTP adjustment): the bounded window re-tracks over time; **slew** the estimate toward a new level rather than jumping, to avoid presentation-time discontinuities.
- **What it does and doesn't buy you:** it gives *relative* correctness for free — correct playback speed and multi-subject/same-sender sync — **without** cross-machine clock sync. **Absolute one-way latency additionally requires NTP-level sync**; without it the "latency" figure conflates skew + delay, so report it as indicative only (or behind an "assume synced clocks" flag) while **jitter and loss are reliable regardless.**

#### A2.c — LiveLink frame-time mapping (UE glue)
- Set each emitted LiveLink frame's `WorldTime` (and `SceneTime`/timecode where available) from the A2.b mapped presentation time, so LiveLink's time-based interpolation works across the network and subjects stay in sync.
- **Fallback:** `tx_wallclock == 0` (legacy) or estimator still warming up → timestamp with local receive time (today's behavior).
- Set a sane default LiveLink subject **buffer offset** covering expected jitter (the presentation-delay knob) and expose it; make sure it doesn't *double* with any O3DB-side delay — measure, don't stack buffers (§2.5).
- **Verify against the target UE version:** exact API to set frame `WorldTime`/`SceneTime` on `FLiveLinkFrameDataStruct` and the `BufferSettings` fields — confirm in the LiveLink source, don't assume.

#### A2.d — Metrics / HUD (`O3DPerformanceMetrics`)
- Per source, surface: latency estimate (caveated per A2.b), jitter (spread of `offset` around the rolling min), loss % and reorder rate (from `gate.Stats()`), duplicate/stale drops, and current gate buffer occupancy. Wire into the existing `o3d.ProfileGuide` HUD.
- Use the atomic-safe update pattern from the Shared metrics fix (#215) — don't reintroduce the `Load()`→compute→`Store()` RMW races for the new counters.

#### A2 — Acceptance
- **Estimator (core, CTest):** synthetic constant skew + bounded jitter → estimated offset converges to `skew + min_delay`; mapped presentation times are monotonic and correctly ordered; a simulated clock step is slewed, not jumped.
- **Integration (B2 replay):** replay a capture with injected jitter/loss into the receiver → HUD reports plausible latency/jitter/loss; **applied pose stream is monotonic (no backward `tx_seq` ever applied)**; with zero loss/jitter the output is identical to today.
- **Legacy:** `tx_seq`/`tx_wallclock == 0` → gate bypassed, receive-time timestamping, behaves exactly as today.
- Estimator verified on Linux CI; receiver wiring via B2 (UE-lite / optional Replay transport) + UE automation on the self-hosted runner (gated on the PR being out of draft).

#### A2 — Open decisions
- Rolling-min window: time-based vs count-based; slew rate on clock step. Start with a few-second window.
- Trust absolute latency (assume NTP) vs report jitter+loss only? **Recommendation: jitter + loss are primary and always shown; absolute latency shown only when clocks are declared synced** (a setting), else labeled "relative."
- Reorder window / buffer offset defaults (tie to A1's `max_window`/`max_delay_s`); expose as console vars.
- Per-source vs per-sender gate keying if multiplexing is ever added.

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

**Goal:** replay a `.o3dscap` capture — through a real transport or straight into a consumer — at real or accelerated speed, with a deterministic loss/jitter/reorder/dup channel model in between. This is the **primary integration-test harness**: A1's `ReorderGate` and C1's concealment are validated by replaying captures through it. **Dependencies:** B1 (reader); interlocks with A1. **Out of scope:** the learned-model training loop (consumes captures, but lives in C3 tooling).

Three cohesive pieces.

#### B2.a — Replay driver (`src/o3ds/replay.*`)
- Reads a capture via the B1 reader and emits frames to a sink `std::function<void(const Frame&)>` (same `Frame { seq, wallclock_s, bytes }` the gate consumes).
- **Timing modes:**
  - *Realtime:* wait the inter-arrival delta `recv_wallclock_us[i+1] − recv_wallclock_us[i]` (clamped ≥ 0), optionally × a `speed` factor.
  - *AFAP (as-fast-as-possible):* ignore timing, emit back-to-back — for deterministic unit tests.
- Optional loop. Purely a source; it does not itself drop/reorder — that's the channel model, kept separate so it can be tested in isolation.

#### B2.b — Channel model (`src/o3ds/channel_model.*`) — the key primitive
A deterministic simulator that sits between the replay source and the sink and reproduces real network pathologies. It is what makes B2 a rigorous test of A1.
- **Config:** `{ uint64_t seed; double loss_prob; double base_latency_s; double max_jitter_s; double dup_prob; }`.
- **Per frame, in a fixed draw order** (loss → jitter → dup, always the same order so a seed is reproducible): drop with `loss_prob`; assign delivery time `emit_t + base_latency + jitter` where `jitter ∈ [0, max_jitter]`; duplicate with `dup_prob`.
- **Reordering emerges physically:** frames are released in **delivery-time order**, not arrival order — a high-jitter frame naturally lands after a later low-jitter one. Implement with a small delivery-time priority queue. (No separate "reorder" knob needed; jitter is the realistic mechanism.)
- **⚠️ Cross-platform determinism trap:** use `std::mt19937_64` seeded explicitly, **but do not use `std::uniform_real_distribution`/`std::normal_distribution`** — their output is *not* specified to be identical across standard-library implementations, which would break "deterministic across runs/platforms." Derive draws manually from the raw generator (e.g. `gen() / (double)UINT64_MAX` for a uniform). Start with a uniform jitter distribution (simple, sufficient); gaussian is a later refinement.
- Exposes ground-truth counters (`dropped`, `duplicated`, `reordered` = frames released out of original seq order) so tests can assert the gate's stats against them.

#### B2.c — Replay CLI (`apps/Replay/`, engine-agnostic, mirrors `apps/Repeater`)
- Args: capture path; target transport URI (`tcp://`, `udp://`, `nng://`, …); timing mode + `speed`; channel params (`--loss`, `--jitter`, `--dup`, `--seed`); `--loop`.
- Links the existing sender/transport code to push post-channel-model frames onto a **real** transport → replay a captured session to a live receiver over any protocol. Turns B2 from a unit-test primitive into a field/integration tool.
- **(Optional / stretch)** a UE-side "Replay" transport registered in the `IOpen3DReceiver` registry, so the plugin can consume a `.o3dscap` as if it were a live source — exercises the receiver apply path / A2 / C1 with no live sender.

#### B2 — Acceptance (core, CTest-wired)
- **Pass-through:** zero-param channel (`loss=jitter=dup=0`) → replay output == capture input, same order and bytes.
- **Determinism:** fixed `seed` + params → byte-identical output event log across two runs (and, given manual distributions, across platforms).
- **Loss-only fed into A1's `ReorderGate`** (`loss=0.05`, `jitter=0`): `gate.Stats().lost == channel.dropped` and `gate.reordered == 0` — crisp because with no jitter there is no reordering.
- **Jitter-only** (`loss=0`, `max_jitter <` gate window): `gate.lost == 0` — bounding jitter below the gate window guarantees every reordering is fully recovered, which is the guarantee that actually matters. **Not** `gate.reordered == channel.reordered` exactly: implementation experience (B2) showed the two counters measure genuinely different things and provably diverge for a specific input shape. `ReorderGate::reordered` only counts frames delivered via its internal drain loop (buffered, then released because a predecessor filled the gap); `ChannelModel::reordered` counts any frame released with a lower seq than one already released. They coincide for a simple two-frame swap, but diverge when one gap gets filled by *multiple separately-arriving* frames — each individually satisfies the gate's normal "next expected seq" path rather than being drained as a batch, so the gate undercounts relative to the channel. The provable, always-true relationship is `gate.reordered <= channel.reordered` given `gate.lost == 0`; assert that instead of exact equality.
- **Combined realism** (`loss=0.05`, `jitter=120 ms`): runs without assertion-fragility; used for A2/C1 HUD/quality checks rather than exact counts.
- **Robustness:** truncated/garbage capture handled by the B1 reader rules (no crash; ASan/UBSan clean).

#### B2 — Open decisions
- Manual vs `std::` distributions — **manual**, for cross-platform determinism (above).
- Jitter shape: uniform to start; gaussian later if realism demands.
- Does the CLI reuse the exact transport-send path used in production (preferred, so it tests real code) vs a thin re-impl? Prefer reuse.

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

### Phase C1 — Receiver-side concealment & latency-hiding

**Goal:** when a frame is missing/late, synthesize a *predicted* pose instead of freezing (hold) or popping; optionally render ahead to hide RTT. **Receiver-only → no cross-peer determinism → the first visible payoff of the whole predictor line.** With zero loss it is a strict no-op (every frame arrives → predictor never invoked → output identical to today). **Dependencies:** C0 (predictor), A2 (gate + clock mapping + HUD), B2 (test harness). **Out of scope:** changing what the sender transmits (C2). **Sender-side changes: none.**

#### C1.a — Concealment state machine (per subject)
Sits on the A2 apply path; the interesting math (blend/correction, error metric) goes **core-side** where it can be Linux-tested, UE holds only the LiveLink push.
- Feed **every applied real frame** into the per-subject `IPosePredictor.Observe()` (§C0).
- Each presentation instant (receiver tick / eval boundary) has a target time `t_pres` from A2.b (+ optional render-ahead horizon, C1.c). Decide the pose at `t_pres`:
  - **Real frame available/coverable** → apply real (let LiveLink interpolate as normal); no prediction.
  - **Starved / durable gap** → `predictor.Predict(t_pres)`. If it returns `true`, push that as a synthetic LiveLink frame **flagged predicted**; if `false` (insufficient history / first frames) → **hold** (today's behavior).
- **Bound the horizon.** After `max_conceal_horizon` (e.g. ~100–200 ms) of pure prediction with no real data, **stop extrapolating and hold** — classical predictors diverge/overshoot on long gaps; a frozen pose beats a flung skeleton. Expose as a cvar.
- Reset the predictor on keyframe / topology change / large seq gap / publisher restart (ties to A1.c re-baseline).

#### C1.b — Correction without rewriting history
- LiveLink samples by time and may already have evaluated past a concealed instant, so **do not try to un-apply or rewrite frames already in LiveLink's buffer.** When real frames resume, `Observe()` them and **correct forward**: blend/damp the applied output from the last-predicted trajectory toward the corrected one over a short window (avoid a hard snap-back "pop"). Keep bookkeeping of which presentation times were already covered by a prediction so you don't double-push.
- **Key design decision (measure first, §2.5) — trigger model:**
  - *Option 1 — gap-triggered (recommended start):* let LiveLink's own interpolation/hold cover small (1–2 frame) gaps; O3DB synthesizes only for gaps/starvation beyond that. Minimal change, leans on LiveLink for the easy cases; the hard part is cleanly detecting "LiveLink is about to starve."
  - *Option 2 — continuous synthesis:* each tick, O3DB provides the pose at `t_pres` (real or predicted) so LiveLink always has a current-time sample and rarely holds. Simpler control flow, no starvation-detection, but pushes synthetic frames throughout gaps.
  - **First measure LiveLink's native gap behavior** (does its interpolation/hold already look acceptable for 1–2 dropped frames?), then pick. Do not build both.

#### C1.c — Latency-hiding / render-ahead (opt-in, default OFF)
- Proactively predict `render_ahead` ms beyond the newest received frame so the displayed pose is closer to "real now," trading accuracy for lower effective latency. Separate mode because it changes the accuracy/latency tradeoff and **interacts with LiveLink's buffer offset (A2.c) — don't double-count the offset.** Off by default; a cvar with a conservative cap.

#### C1.d — Metrics (feeds A2.d HUD)
- **Prediction error** (the important one): when a real frame arrives for a time that was concealed, compare the earlier prediction to the actual → per-joint + aggregate error. **This number is the go/no-go signal for C3** (is a learned predictor worth it?).
- Concealed-frame count/rate, fallback-to-hold count, horizon distribution, and a **"pop" metric** = inter-frame applied-pose discontinuity at gap-recovery boundaries (this is what concealment is meant to reduce vs hold).

#### C1 — Acceptance (mostly via B2 — the capture is ground truth)
- **The elegant test:** replay a clean capture through B2 with **loss-only** injection. The dropped frames' true poses are known (they're in the capture), so C1's prediction can be scored directly: assert aggregate **prediction error < bound** and the **pop metric < the `HoldPredictor` baseline** on the same dropped set.
- **Baseline comparison:** `LinearPredictor` concealment error < `HoldPredictor` (== today) on the same dropped frames — validates the whole premise on real motion.
- **No-op safety:** zero-loss replay → prediction never triggers → output byte-identical to today.
- **Horizon bound:** a long injected outage → prediction stops at `max_conceal_horizon` and holds (no runaway extrapolation).
- Core math (error, blend) unit-tested on Linux; LiveLink injection via B2 (UE-lite / optional Replay transport) + UE automation on the self-hosted runner (gated on the PR being out of draft).

#### C1 — Open decisions
- Trigger model Option 1 vs 2 (above) — measure LiveLink first.
- `max_conceal_horizon`, `render_ahead` defaults and caps; blend/correction window length.
- Which predictor is the C1 default (`Linear` almost certainly; `Quadratic` may overshoot — evaluate on captures).

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

## 6. Workstream D — Wire Efficiency

### Phase D1 — Adaptive/variable-bit channel quantization (wire-format, transport- and prediction-agnostic)

- **Goal:** shrink bytes/frame on the wire encoding itself, independent of whether a channel is predicted (C0–C2) or held (today). Today's `Subject::SerializeUpdate` (`src/o3ds/model.cpp`) sends each changed channel as full 32-bit floats once its `delta() > deltaThreshold` — an all-or-nothing "send at full precision, or send nothing" decision per channel, with **no quantization at all today**. Variable-bit quantization sends fewer bits for a channel moving slowly/predictably and holds full precision for one moving fast — a standard motion-codec technique that **stacks with C2 rather than competing with it** (a residual is just another value to quantize, same as a raw delta is today).
- **Why this first:** unlike C2, this never touches predictor history, so it carries none of C2's history-divergence-under-loss risk (§ Phase C2) — it's a strictly per-frame, stateless-across-loss encoding change. That means it applies uniformly to **both reliable and unreliable transports**, including the ones C2 explicitly excludes (UDP, MoQ datagrams) — the one concrete "make unreliable-transport streaming cheaper/better" lever identified so far that doesn't require solving C2's divergence problem first.
- **Approach:**
  - Per-channel (translation/rotation/scale/curve) variable bit-depth chosen from the channel's own recent delta magnitude (small movement → fewer bits; large/fast movement → up to today's full-float ceiling) instead of today's fixed all-or-nothing threshold.
  - Encode the chosen precision (a small enumerated class, not a raw bit count) alongside each channel update so it's self-describing per record — no session-level renegotiation needed when a joint suddenly starts moving fast mid-stream.
  - Rotations need their own care: naive per-component quantization of a quaternion can denormalize it — quantize via axis-angle or a "smallest-three" representation instead so decode always renormalizes cleanly. Treat this as its own accuracy check, separate from translation/scale.
  - `deltaThreshold` keeps its existing role as the "send nothing" floor; quantization only decides *how precisely* to encode a channel that already cleared that floor.
- **Files/modules:** `src/o3ds/model.cpp`/`model.h` (`Subject::SerializeUpdate`/`ParseUpdate` — the same functions C2's residual generalization touches, so sequence the two changes to not conflict), schema (`o3ds.fbs` — new per-channel precision field/enum).
- **Acceptance:** bytes/frame drop measurably vs today's fixed-float encoding on a representative capture corpus (reuse B2's replay harness), within a bounded per-channel reconstruction-error budget (analogous to C1's prediction-error metric); encode→decode round-trip fidelity unit-tested in core; benefit holds whether or not C2 is active (applies equally to a raw delta or a C2 residual).
- **Dependencies:** none blocking — independent of C0–C3; can land before, after, or in parallel with C2. **Out of scope:** a full entropy coder (arithmetic/range coding) — variable-bit quantization only; that would be a further follow-on if this proves out. Cross-peer prediction is C2's concern, not this phase's.
- **Open decisions:** exact bit-depth tiers and how a channel's tier is chosen (fixed thresholds on delta magnitude first, same "classical baseline before anything adaptive/learned" posture as C0); how quaternion quantization error interacts with C1's correction-blend math (`QuatSlerpShortestPath`) — rounding noise inside a concealment correction window needs to stay well under the "pop" metric's threshold.

---

## 7. Sequencing & dependency graph

Node tags: **(core)** = engine-agnostic, Linux/CTest-testable; **(core+UE)** = core logic + thin UE glue; **(UE)** = plugin glue.

```
A1 (seq/clock + gate, core) ──┬──► A2 (receiver integ., core+UE)
                              │
                              ├──► C0 (predictor + baselines, core) ──► C1 (concealment, core+UE)
                              │                                          │
B1 (capture fmt, core) ──► B2 (replay + channel, core+UE) ──► (test fixtures for A2, C1)
        │                                                              │
        └──► B3 (repeater capture, opt)                                ▼
                                                   C2 (residual compression, core+UE) ──► C3 (learned)

D1 (adaptive quantization, core+UE) ── independent of A/B/C; can run in parallel with any of them
```

Dependencies: A2 ← A1. C0 ← A1. C1 ← C0, A2, B2. B2 ← B1. C2 ← C0, A1 (evaluate *after* C1). C3 ← C0–C2, B. D1 ← none (touches the same `model.cpp` functions as C2, so sequence the two to avoid merge conflicts, but neither blocks the other). (The C1→C2 line is sequencing, not a hard dependency.)

Recommended order: **A1 → B1 → B2 → A2 → C0 → C1** (this delivers resilience + concealment + the test harness), then evaluate **C2**, then a gated **C3** spike. **D1** can be picked up whenever convenient — it has no dependencies and no history-divergence risk, so it's a good fill-in between the sequenced phases above. A1, B1, C0, and D1 are independent enough to run in parallel by separate agents.

---

## 8. Testing strategy

- **Core-first (§0.1):** sequencing + reorder gate (A1), capture round-trip (B1), channel model (B2), clock-offset estimator (A2.b), predictor math (C0), residual round-trip (C2), channel quantization round-trip (D1) are all pure C++ — unit-test in the core and **wire into CTest so the Linux CI actually runs them.** Reuse the ASan/UBSan harness pattern already used for the parser-hardening work; feed malformed inputs to every new parser/reader.
- **Replay-as-test (B2):** the primary integration harness. Golden `.o3dscap` sessions + seeded channel models give deterministic, UE-free (or UE-lite) regression tests for A2 and C1.
- **UE automation:** extend the existing automation suites for the receiver apply-path changes; run them on the self-hosted runner (note: gated on the PR being out of draft).
- Every phase's acceptance criteria above names its concrete test.

---

## 9. Risks & open questions (consolidated)

- **Core duplication (#203)** — resolve early or every core change is doubled (§0.2).
- **History divergence under loss (C2)** — residual coding silently breaks on lossy transports because the receiver's pose history diverges from the sender's; scope C2 to reliable/ordered transports (or use keyframe-relative independent deltas), and let C1 concealment carry unreliable links. Load-bearing — see C2.
- **Float determinism** for residual coding (C2) — measure drift before committing; keyframe cadence / fixed-point as mitigations.
- **LiveLink division of labor** — RESOLVED (§2.5): LiveLink owns presentation smoothing; O3DB owns the network layer + clock-offset→timestamp mapping + optional predicted-frame concealment. Verify the exact LiveLink buffer/time APIs against the target UE version before A2.
- **Clock-offset estimation** (§2.5, A2) — mapping `tx_wallclock_us` to local engine time needs a robust moving/min estimate of the send→recv offset; absolute one-way latency additionally needs NTP-level sync, else report relative jitter/loss only.
- **Inference budget** for the learned model (C3) — hard real-time constraint; keep C3 a gated spike.
- **Quaternion quantization error vs. C1's correction blend (D1)** — rounding noise from a quantized rotation must stay well under the "pop" metric's threshold, or D1 could visibly fight with C1's own smoothing; verify empirically before tightening bit-depth tiers.
- **Two-copy schema edits** until #203 lands.

## 10. Definition of done (program level)

- `tx_seq`/`tx_wallclock_us` shipped, populated, and consumed; legacy interop preserved.
- Receiver reorders/dedups/drops-stale and reports latency/jitter/loss on the HUD.
- Capture/replay format + CLI exist; replay drives deterministic tests.
- A pluggable predictor with classical baselines conceals loss/hides latency on the receiver, measured against real frames.
- (Stretch) residual compression reduces bytes/frame at equal visual error; (research) learned predictor beats baselines within budget.
- Adaptive channel quantization reduces bytes/frame on both reliable and unreliable transports at a bounded reconstruction-error cost.
- New core logic is unit-tested and running in CI.
